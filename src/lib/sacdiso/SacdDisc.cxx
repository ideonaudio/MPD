// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SacdDisc.hxx"
#include "Domain.hxx"
#include "tag/Handler.hxx"
#include "tag/Type.hxx"
#include "Log.hxx"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "lib/icu/Converter.hxx"
#include "util/AllocatedString.hxx"

namespace Sacd {

namespace {

// Audio sector header size
constexpr std::size_t kAudioSectorHeaderSize = 1;  // Only 1 byte!
constexpr std::size_t kAudioPacketInfoSize = 2;
constexpr std::size_t kAudioFrameInfoSizeDst = 4;  // For DST: timecode + channel byte
constexpr std::size_t kAudioFrameInfoSizeDsd = 3;  // For DSD: only timecode

// Character set names for iconv
constexpr const char* kCharsetNames[] = {
	"ISO-8859-1",  // Iso646 (fallback to Latin-1)
	"ISO-8859-1",  // Iso8859_1
	"EUC-KR",      // Korean
	"GB2312",      // Simplified Chinese
	"BIG5",        // Traditional Chinese
	"SHIFT_JIS",   // Japanese
	"KOI8-R",      // Russian
	"ISO-8859-1",  // Iso8859_1_2
};

} // anonymous namespace

Disc::~Disc() noexcept
{
	Close();
}

bool
Disc::Open(std::unique_ptr<Media> media)
{
	Close();

	if (!media || !media->IsValid()) {
		FmtDebug(sacdiso_domain, "Disc::Open - media is null or invalid");
		return false;
	}

	// Detect sector size
	sector_size_ = DetectSectorSize(*media);
	if (sector_size_ == 0) {
		FmtDebug(sacdiso_domain, "Disc::Open - DetectSectorSize failed");
		return false;
	}

	// Set sector offset for PSN format
	sector_offset_ = (sector_size_ == kPsnSize) ? 12 : 0;

	media_ = std::move(media);

	// Read Master TOC
	if (!ReadMasterToc()) {
		FmtDebug(sacdiso_domain, "Disc::Open - ReadMasterToc failed");
		Close();
		return false;
	}

	return true;
}

void
Disc::Close() noexcept
{
	media_.reset();
	sector_size_ = 0;
	sector_offset_ = 0;
	disc_info_ = DiscInfo{};
	current_area_ = AreaId::Stereo;
	current_track_ = 0;
	track_start_lsn_ = 0;
	track_length_lsn_ = 0;
	current_lsn_ = 0;
	/* release the playback buffers */
	audio_state_ = AudioSectorState{};
	frame_state_ = FrameState{};
}

std::size_t
Disc::GetTrackCount(AreaId area_id) const noexcept
{
	return disc_info_.GetArea(area_id).GetTrackCount();
}

std::size_t
Disc::GetTotalTrackCount() const noexcept
{
	return GetTrackCount(AreaId::Stereo) + GetTrackCount(AreaId::Multichannel);
}

double
Disc::GetTrackDuration(AreaId area_id, std::size_t track_index) const noexcept
{
	const auto& area = disc_info_.GetArea(area_id);
	if (track_index >= area.tracks.size())
		return 0.0;
	return area.tracks[track_index].duration.ToSeconds();
}

unsigned
Disc::GetChannelCount(AreaId area_id) const noexcept
{
	return disc_info_.GetArea(area_id).channel_count;
}

bool
Disc::IsDstEncoded(AreaId area_id) const noexcept
{
	return disc_info_.GetArea(area_id).IsDstEncoded();
}

void
Disc::GetTrackInfo(AreaId area_id, std::size_t track_index,
                   TagHandler& handler) const noexcept
{
	const auto& area = disc_info_.GetArea(area_id);
	if (track_index >= area.tracks.size())
		return;

	const auto& track = area.tracks[track_index];
	const auto& master = disc_info_.master_text;

	// Track number
	handler.OnTag(TAG_TRACK, std::to_string(track_index + 1).c_str());

	// Duration
	handler.OnDuration(SongTime::FromMS(
		static_cast<unsigned>(track.duration.ToSeconds() * 1000)));

	// Album info
	if (!master.album_title.empty()) {
		std::string album = master.album_title;
		album += " (";
		album += (area_id == AreaId::Stereo) ? "2CH" : "MCH";
		album += "-";
		album += area.IsDstEncoded() ? "DST" : "DSD";
		album += ")";
		handler.OnTag(TAG_ALBUM, album.c_str());
	}

	if (!master.album_artist.empty())
		handler.OnTag(TAG_ARTIST, master.album_artist.c_str());

	// Track info
	if (!track.text.title.empty())
		handler.OnTag(TAG_TITLE, track.text.title.c_str());

	if (!track.text.performer.empty())
		handler.OnTag(TAG_PERFORMER, track.text.performer.c_str());

	if (!track.text.composer.empty())
		handler.OnTag(TAG_COMPOSER, track.text.composer.c_str());

	// Date
	if (disc_info_.disc_year > 0)
		handler.OnTag(TAG_DATE, std::to_string(static_cast<unsigned>(disc_info_.disc_year)).c_str());

	// Disc number
	if (disc_info_.album_set_size > 1 && disc_info_.album_sequence_number > 0)
		handler.OnTag(TAG_DISC,
		              std::to_string(static_cast<unsigned>(disc_info_.album_sequence_number)).c_str());
}

void
Disc::SelectArea(AreaId area_id) noexcept
{
	current_area_ = area_id;
}

bool
Disc::SelectTrack(std::size_t track_index, uint32_t offset) noexcept
{
	const auto& area = disc_info_.GetArea(current_area_);
	if (track_index >= area.tracks.size()) {
		FmtDebug(sacdiso_domain, "SelectTrack - track {} out of range (size={})",
		           track_index, area.tracks.size());
		return false;
	}

	current_track_ = track_index;
	const auto& track = area.tracks[track_index];

	if (!edited_master_mode_) {
		track_start_lsn_ = track.start_lsn;
		track_length_lsn_ = track.length_lsn;
	} else {
		// Edited master mode: use next track's start as end
		track_start_lsn_ = (track_index > 0)
			? track.start_lsn
			: area.track_start;

		const bool has_next = track_index + 1 < area.tracks.size();
		const uint32_t next_start = has_next
			? area.tracks[track_index + 1].start_lsn
			: area.track_end;

		/* a track list which is not monotonically increasing
		   would underflow the subtraction below */
		if (next_start < track_start_lsn_) {
			return false;
		}

		track_length_lsn_ = next_start - track_start_lsn_;
		if (has_next &&
		    track_length_lsn_ < std::numeric_limits<uint32_t>::max())
			++track_length_lsn_;
	}

	/* reject a starting offset which would overflow the LSN */
	if (offset > std::numeric_limits<uint32_t>::max() - track_start_lsn_) {
		FmtDebug(sacdiso_domain,
		         "SelectTrack - starting offset {} overflows LSN", offset);
		return false;
	}

	current_lsn_ = track_start_lsn_ + offset;

	/* Allocate the playback buffers on first use and reuse them for
	   every later seek.  Doing this here rather than in the
	   constructor keeps the scanning code paths, which never select
	   a track, free of these 66 kB. */
	if (frame_state_.data.empty()) {
		try {
			audio_state_.sector_buffer.resize(kPsnSize);
			frame_state_.data.resize(FrameState::kMaxFrameSize);
		} catch (...) {
			/* this method is noexcept */
			return false;
		}
	}

	audio_state_.Reset();
	frame_state_.Reset();

	// Seek to start position
	return media_->Seek(static_cast<uint64_t>(current_lsn_) * sector_size_);
}

bool
Disc::ReadFrame(std::span<std::byte> buffer, std::size_t& frame_size,
                FrameType& frame_type) noexcept
{

	/* compute the track end in 64 bit so that a corrupt track
	   length cannot wrap the sum around */
	const uint64_t track_end_lsn =
		static_cast<uint64_t>(track_start_lsn_) + track_length_lsn_;

	while (current_lsn_ < track_end_lsn) {
		// Need to read a new sector?
		// Limit check to actual parsed packets (max 8)
		const uint8_t effective_packet_count = std::min(audio_state_.packet_count,
		                                                 static_cast<uint8_t>(audio_state_.packets.size()));
		if (audio_state_.current_packet >= effective_packet_count) {
			// Read next sector
			if (!ReadRawSector(current_lsn_, audio_state_.sector_buffer)) {
				FmtDebug(sacdiso_domain, "ReadFrame - ReadRawSector failed at lsn={}", current_lsn_);
				return false;
			}

			++current_lsn_;

			// Parse audio sector header
			const std::byte* data = audio_state_.sector_buffer.data() + sector_offset_;
			const auto* header = reinterpret_cast<const AudioSectorHeader*>(data);

			audio_state_.dst_encoded = header->IsDstEncoded();
			audio_state_.packet_count = header->GetPacketInfoCount();
			audio_state_.frame_count = header->GetFrameInfoCount();
			audio_state_.current_packet = 0;
			audio_state_.buffer_offset = kAudioSectorHeaderSize;

			// Parse packet info FIRST
			for (uint8_t i = 0; i < audio_state_.packet_count && i < 8; ++i) {
				std::memcpy(&audio_state_.packets[i],
				            data + audio_state_.buffer_offset,
				            kAudioPacketInfoSize);
				audio_state_.buffer_offset += kAudioPacketInfoSize;
			}

			// Parse frame info
			const std::size_t frame_info_size = audio_state_.dst_encoded
				? kAudioFrameInfoSizeDst : kAudioFrameInfoSizeDsd;
			for (uint8_t i = 0; i < audio_state_.frame_count && i < 8; ++i) {
				std::memcpy(&audio_state_.frames[i],
				            data + audio_state_.buffer_offset,
				            frame_info_size);
				audio_state_.buffer_offset += frame_info_size;
			}
		}

		// Process packets (limit to array size to prevent out-of-bounds access)
		const uint8_t max_packets = std::min(audio_state_.packet_count,
		                                     static_cast<uint8_t>(audio_state_.packets.size()));
		while (audio_state_.current_packet < max_packets) {
			const auto& packet = audio_state_.packets[audio_state_.current_packet];
			const std::byte* sector_data = audio_state_.sector_buffer.data()
			                               + sector_offset_;
			const uint16_t packet_len = packet.GetPacketLength();

			/* A packet which extends past the end of the sector
			   means the sector header is corrupt; drop the rest of
			   this sector and continue with the next one.  The
			   header parser above bounds buffer_offset to 43 bytes
			   and this check keeps it within the sector
			   afterwards, so the subtraction cannot underflow. */
			if (packet_len > kLsnSize - audio_state_.buffer_offset) {
				audio_state_.current_packet = max_packets;
				break;
			}

			switch (packet.GetDataType()) {
			case DataType::Audio:
				if (frame_state_.started) {
					if (packet.IsFrameStart()) {
						// Complete frame ready
						if (frame_state_.size <= buffer.size()) {
							std::memcpy(buffer.data(),
							            frame_state_.data.data(),
							            frame_state_.size);
							frame_size = frame_state_.size;
							frame_type = frame_state_.dst_encoded
								? FrameType::Dst : FrameType::Dsd;
							// Log first frame
							static bool logged_first_frame = false;
							if (!logged_first_frame) {
								logged_first_frame = true;
							}
							frame_state_.started = false;
							return true;
						}
						// Buffer too small
						FmtDebug(sacdiso_domain, "Frame buffer too small: frame={}, buffer={}",
						           frame_state_.size, buffer.size());
						frame_state_.started = false;
						frame_type = FrameType::Invalid;
						return true;
					}
				} else {
					if (packet.IsFrameStart()) {
						frame_state_.size = 0;
						frame_state_.dst_encoded = audio_state_.dst_encoded;
						frame_state_.started = true;
					}
				}

				if (frame_state_.started) {
					if (frame_state_.size + packet_len <= FrameState::kMaxFrameSize) {
						std::memcpy(frame_state_.data.data() + frame_state_.size,
						            sector_data + audio_state_.buffer_offset,
						            packet_len);
						frame_state_.size += packet_len;
					}
				}
				break;

			case DataType::Supplementary:
			case DataType::Padding:
				// Skip these packet types
				break;
			}

			audio_state_.buffer_offset += packet_len;
			++audio_state_.current_packet;
		}
	}

	// End of track - return any pending frame
	if (frame_state_.started && frame_state_.size <= buffer.size()) {
		std::memcpy(buffer.data(), frame_state_.data.data(), frame_state_.size);
		frame_size = frame_state_.size;
		frame_type = frame_state_.dst_encoded ? FrameType::Dst : FrameType::Dsd;
		frame_state_.started = false;
		return true;
	}

	frame_type = FrameType::Invalid;
	return false;
}

bool
Disc::Seek(double seconds) noexcept
{
	const double duration = GetDuration();

	/* the negated comparisons also reject NaN; converting an
	   out-of-range double to an integer is undefined behaviour */
	if (!(duration > 0) || !(seconds >= 0) || sector_size_ == 0)
		return false;

	if (seconds > duration)
		seconds = duration;

	/* dividing first keeps the factor within [0,1], so the
	   conversion below cannot exceed the track size */
	const uint64_t offset =
		static_cast<uint64_t>(GetSize() * (seconds / duration));
	const uint32_t lsn_offset = static_cast<uint32_t>(offset / sector_size_);

	return SelectTrack(current_track_, lsn_offset);
}

uint64_t
Disc::GetPosition() const noexcept
{
	if (current_lsn_ < track_start_lsn_)
		return 0;

	return static_cast<uint64_t>(current_lsn_ - track_start_lsn_) * sector_size_;
}

uint64_t
Disc::GetSize() const noexcept
{
	return static_cast<uint64_t>(track_length_lsn_) * sector_size_;
}

double
Disc::GetDuration() const noexcept
{
	return GetTrackDuration(current_area_, current_track_);
}

bool
Disc::ReadMasterToc()
{
	// Allocate buffer for Master TOC
	std::vector<std::byte> master_data(kMasterTocLength * kLsnSize);


	if (!ReadRawSectors(kMasterTocStart, kMasterTocLength, master_data)) {
		FmtDebug(sacdiso_domain, "ReadMasterToc: ReadRawSectors failed");
		return false;
	}

	// Parse Master TOC header
	const auto* master_toc = reinterpret_cast<const MasterToc*>(master_data.data());

	// Log raw signature bytes

	if (!master_toc->IsValid()) {
		FmtDebug(sacdiso_domain, "ReadMasterToc: Invalid signature (expected SACDMTOC)");
		return false;
	}

	// Check version - FIXED: check major first, then minor only if major is equal

	if (master_toc->version.major > kSupportedVersionMajor ||
	    (master_toc->version.major == kSupportedVersionMajor &&
	     master_toc->version.minor > kSupportedVersionMinor)) {
		FmtDebug(sacdiso_domain, "ReadMasterToc: Unsupported SACD version");
		return false;
	}

	// Store disc info
	disc_info_.version = master_toc->version;
	disc_info_.album_set_size = static_cast<uint16_t>(master_toc->album_set_size);
	disc_info_.album_sequence_number = static_cast<uint16_t>(master_toc->album_sequence_number);
	disc_info_.disc_year = static_cast<uint16_t>(master_toc->disc_date_year);
	disc_info_.disc_month = master_toc->disc_date_month;
	disc_info_.disc_day = master_toc->disc_date_day;
	disc_info_.is_hybrid = master_toc->disc_type_hybrid != 0;

	// Parse Master Text (first language only)
	const auto* master_text = reinterpret_cast<const MasterText*>(
		master_data.data() + kLsnSize);
	if (master_text->IsValid()) {
		const auto charset = static_cast<CharacterSet>(
			master_toc->locales[0].character_set & 0x07);
		const char* text_base = reinterpret_cast<const char*>(master_text);

		auto extract_text = [&](uint16_t offset) -> std::string {
			/* the text positions are byte offsets into this
			   single sector, and each string has to be
			   null-terminated before the end of it */
			if (offset == 0 || offset >= kLsnSize)
				return {};

			const char* const str = text_base + offset;
			const auto* const nul = static_cast<const char*>(
				std::memchr(str, '\0', kLsnSize - offset));
			if (nul == nullptr)
				/* unterminated string */
				return {};

			return ConvertCharset(str, nul - str, charset);
		};

		disc_info_.master_text.album_title = extract_text(
			static_cast<uint16_t>(master_text->album_title_position));
		disc_info_.master_text.album_artist = extract_text(
			static_cast<uint16_t>(master_text->album_artist_position));
		disc_info_.master_text.album_publisher = extract_text(
			static_cast<uint16_t>(master_text->album_publisher_position));
		disc_info_.master_text.album_copyright = extract_text(
			static_cast<uint16_t>(master_text->album_copyright_position));
		disc_info_.master_text.disc_title = extract_text(
			static_cast<uint16_t>(master_text->disc_title_position));
		disc_info_.master_text.disc_artist = extract_text(
			static_cast<uint16_t>(master_text->disc_artist_position));
	}

	// Read Area TOCs
	const uint32_t area1_start = static_cast<uint32_t>(master_toc->area_1_toc_1_start);
	const uint16_t area1_size = static_cast<uint16_t>(master_toc->area_1_toc_size);

	if (area1_start != 0 && area1_size != 0) {
		if (!ReadAreaToc(AreaId::Stereo, area1_start, area1_size)) {
			FmtDebug(sacdiso_domain, "ReadMasterToc: ReadAreaToc(stereo) failed");
		}
	}

	const uint32_t area2_start = static_cast<uint32_t>(master_toc->area_2_toc_1_start);
	const uint16_t area2_size = static_cast<uint16_t>(master_toc->area_2_toc_size);

	if (area2_start != 0 && area2_size != 0) {
		if (!ReadAreaToc(AreaId::Multichannel, area2_start, area2_size)) {
			FmtDebug(sacdiso_domain, "ReadMasterToc: ReadAreaToc(mch) failed");
		}
	}


	return disc_info_.HasStereoArea() || disc_info_.HasMultichannelArea();
}

bool
Disc::ReadAreaToc([[maybe_unused]] AreaId area_id, uint32_t toc_start, uint16_t toc_size)
{
	/* Both values are read from the disc and are not trustworthy.
	   ReadRawSectors() would fail on a short read anyway, but only
	   after allocating up to 128 MB for a bogus size, so reject an
	   out-of-range TOC before allocating anything.  The operands are
	   widened to 64 bit because the product would otherwise
	   overflow. */
	if (toc_size == 0 ||
	    (static_cast<uint64_t>(toc_start) + toc_size) * sector_size_ >
	    media_->GetSize())
		return false;

	std::vector<std::byte> area_data(static_cast<std::size_t>(toc_size) * kLsnSize);

	if (!ReadRawSectors(toc_start, toc_size, area_data))
		return false;

	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data.data());
	if (!area_toc->IsValid())
		return false;

	// Determine which area this is
	AreaId actual_area_id = area_toc->IsTwoChannel()
		? AreaId::Stereo : AreaId::Multichannel;

	auto& area = disc_info_.GetArea(actual_area_id);
	area.id = actual_area_id;
	area.channel_count = area_toc->channel_count;
	area.loudspeaker_config = static_cast<LoudspeakerConfig>(area_toc->loudspeaker_config);
	area.frame_format = area_toc->IsDstEncoded() ? FrameFormat::Dst : FrameFormat::Dsd_3_in_16;
	area.track_start = static_cast<uint32_t>(area_toc->track_start);
	area.track_end = static_cast<uint32_t>(area_toc->track_end);

	// Parse track list and text
	ReadTrackList(area, area_data.data(), area_data.size());
	ReadTrackText(area, area_data.data(), area_data.size());

	return true;
}

bool
Disc::ReadTrackList(AreaInfo& area, const std::byte* area_data, std::size_t area_size)
{
	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data);
	const uint8_t track_count = area_toc->track_count;


	if (track_count == 0)
		return false;

	// Find SACDTRL1 (track offsets)
	const std::byte* ptr = area_data + kLsnSize;
	const std::byte* end = area_data + area_size;
	bool found_trl1 = false;

	while (ptr + kLsnSize <= end) {
		const auto* trl1 = reinterpret_cast<const TrackListOffset*>(ptr);

		if (trl1->IsValid()) {
			found_trl1 = true;

			// SACDTRL1 layout (from scarletbook.h):
			//   offset 0-7:    "SACDTRL1" signature (8 bytes)
			//   offset 8-1027: track_start_lsn[255] - ALL start LSNs first (255 * 4 = 1020 bytes)
			//   offset 1028-2047: track_length_lsn[255] - ALL length LSNs after (255 * 4 = 1020 bytes)
			// NOT interleaved!

			/* the loop condition guarantees a full sector, and
			   both arrays together fill it exactly, so indexing
			   them with a uint8_t track_count stays in bounds */
			static_assert(8 + 2 * kMaxTracks * sizeof(PackedBE32) <= kLsnSize,
			              "SACDTRL1 arrays do not fit into one sector");

			const auto* start_lsn_array = reinterpret_cast<const PackedBE32*>(ptr + 8);
			const auto* length_lsn_array = reinterpret_cast<const PackedBE32*>(
				ptr + 8 + kMaxTracks * sizeof(PackedBE32));

			area.tracks.resize(track_count);
			for (uint8_t i = 0; i < track_count; ++i) {
				area.tracks[i].start_lsn = static_cast<uint32_t>(start_lsn_array[i]);
				area.tracks[i].length_lsn = static_cast<uint32_t>(length_lsn_array[i]);
			}
			break;
		}
		ptr += kLsnSize;
	}

	if (!found_trl1)
		LogDebug(sacdiso_domain, "no SACDTRL1 signature found");

	// Find SACDTRL2 (track times)
	ptr = area_data + kLsnSize;
	while (ptr + kLsnSize <= end) {
		const auto* trl2 = reinterpret_cast<const TrackListTime*>(ptr);
		if (trl2->IsValid()) {

			// SACDTRL2 layout (from scarletbook.h):
			//   offset 0-7:    "SACDTRL2" signature (8 bytes)
			//   offset 8-1027: start[255] - start times (255 * 4 = 1020 bytes)
			//   offset 1028-2047: duration[255] - durations (255 * 4 = 1020 bytes)
			// TrackTimeDuration is 4 bytes (minutes, seconds, frames, flags)
			static_assert(8 + 2 * kMaxTracks * sizeof(TrackTimeDuration) <= kLsnSize,
			              "SACDTRL2 arrays do not fit into one sector");

			const auto* duration_array = reinterpret_cast<const TrackTimeDuration*>(
				ptr + 8 + kMaxTracks * sizeof(TrackTimeDuration));

			for (uint8_t i = 0; i < track_count && i < area.tracks.size(); ++i) {
				// Copy from on-disc format to runtime format
				area.tracks[i].duration.minutes = duration_array[i].minutes;
				area.tracks[i].duration.seconds = duration_array[i].seconds;
				area.tracks[i].duration.frames = duration_array[i].frames;
			}
			break;
		}
		ptr += kLsnSize;
	}

	return !area.tracks.empty();
}

bool
Disc::ReadTrackText(AreaInfo& area, const std::byte* area_data, std::size_t area_size)
{
	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data);
	const uint8_t track_count = area_toc->track_count;

	if (track_count == 0 || area.tracks.empty())
		return false;

	// Get character set from first locale
	const auto charset = static_cast<CharacterSet>(
		area_toc->languages[0].character_set & 0x07);

	// Search for SACDTTxt signature in area data
	const std::byte* ptr = area_data + kLsnSize;  // Skip first sector (AreaToc header)
	const std::byte* end = area_data + area_size;
	bool found_text = false;

	/* The position array is indexed with a uint8_t track_count, so
	   requiring a full sector here keeps those reads in bounds. */
	static_assert(8 + kMaxTracks * sizeof(PackedBE16) <= kLsnSize,
	              "SACDTTxt position array does not fit into one sector");

	while (ptr + kLsnSize <= end) {
		const auto* text_header = reinterpret_cast<const TrackTextHeader*>(ptr);
		if (text_header->IsValid()) {
			found_text = true;

			// Calculate total bytes available from SACDTTxt start to end of area data
			const std::size_t remaining_bytes =
				static_cast<std::size_t>(end - ptr);

			// Position array starts at offset 8 (after signature)
			const auto* positions = reinterpret_cast<const PackedBE16*>(ptr + 8);


			// Parse text for each track.
			// On-disc layout per record (verified empirically against
			// multiple commercial SACD ISOs):
			//   byte 0:    track_amount (number of text items)
			//   bytes 1-3: reserved (00 00 00)
			//   bytes 4+:  N items, each:
			//                1 byte  type marker (01=title, 02=performer,
			//                                    03=songwriter, ...)
			//                chars   null-terminated string with literal
			//                        leading space
			//                bytes   0-3 bytes of zero padding before next
			//                        item
			// We scan forward locating null-terminated printable strings
			// rather than relying on a fixed offset table.
			for (uint8_t i = 0; i < track_count && i < area.tracks.size(); ++i) {
				const uint16_t text_pos = static_cast<uint16_t>(positions[i]);

				if (text_pos == 0 || text_pos >= remaining_bytes)
					continue;

				// Bound this track's record by the next track's
				// start position, falling back to the end of the
				// SACDTTxt block.
				std::size_t rec_end_offset = remaining_bytes;
				for (uint8_t k = i + 1; k < track_count; ++k) {
					const uint16_t next_pos =
						static_cast<uint16_t>(positions[k]);
					if (next_pos > text_pos &&
					    next_pos < remaining_bytes) {
						rec_end_offset = next_pos;
						break;
					}
				}

				if (text_pos + sizeof(TrackTextRecord) > rec_end_offset)
					continue;

				const std::byte* rec_start = ptr + text_pos;
				const std::byte* rec_end = ptr + rec_end_offset;

				const auto* record =
					reinterpret_cast<const TrackTextRecord*>(rec_start);
				const uint8_t text_amount = record->track_amount;


				if (text_amount == 0 || text_amount > 6)
					continue;

				// Skip 4-byte record header, then scan forward
				// extracting null-terminated strings.
				const std::byte* item_ptr =
					rec_start + sizeof(TrackTextRecord);
				uint8_t emitted = 0;

				while (emitted < text_amount && item_ptr < rec_end) {
					// Skip non-printable bytes (type marker + padding)
					while (item_ptr < rec_end) {
						const uint8_t b =
							static_cast<uint8_t>(*item_ptr);
						// Printable ASCII or extended (high bit set)
						if (b >= 0x20 && b != 0x7F)
							break;
						++item_ptr;
					}
					if (item_ptr >= rec_end)
						break;

					// Read null-terminated string
					const char* str_start =
						reinterpret_cast<const char*>(item_ptr);
					while (item_ptr < rec_end &&
					       *item_ptr != std::byte{0})
						++item_ptr;

					const std::size_t str_len =
						reinterpret_cast<const char*>(item_ptr) -
						str_start;

					if (str_len == 0)
						break;

					// Strip the literal leading space that the SACD
					// format prefixes to every text item
					const char* clean_start = str_start;
					std::size_t clean_len = str_len;
					if (clean_len > 0 && *clean_start == ' ') {
						++clean_start;
						--clean_len;
					}

					if (clean_len > 0) {
						std::string text = ConvertCharset(
							clean_start, clean_len, charset);

						switch (emitted) {
						case 0:
							area.tracks[i].text.title =
								std::move(text);
							break;
						case 1:
							area.tracks[i].text.performer =
								std::move(text);
							break;
						case 2:
							area.tracks[i].text.songwriter =
								std::move(text);
							break;
						case 3:
							area.tracks[i].text.composer =
								std::move(text);
							break;
						case 4:
							area.tracks[i].text.arranger =
								std::move(text);
							break;
						case 5:
							area.tracks[i].text.message =
								std::move(text);
							break;
						}
					}

					++emitted;

					// Advance past the null terminator
					if (item_ptr < rec_end)
						++item_ptr;
				}
			}
			break;  // Found and processed SACDTTxt
		}
		ptr += kLsnSize;
	}

	if (!found_text)
		LogDebug(sacdiso_domain, "no SACDTTxt signature found");

	return found_text;
}

std::string
Disc::ConvertCharset(const char* data, std::size_t length, CharacterSet charset)
{
	const std::string_view src{data, length};

#ifdef HAVE_ICU_CONVERTER
	const auto charset_idx = static_cast<std::size_t>(charset);
	if (charset_idx >= std::size(kCharsetNames))
		return std::string{src};

	try {
		const auto converter =
			IcuConverter::Create(kCharsetNames[charset_idx]);
		return std::string{converter->ToUTF8(src).c_str()};
	} catch (...) {
		/* unsupported charset or malformed input: pass the
		   bytes through unchanged */
		return std::string{src};
	}
#else
	return std::string{src};
#endif
}

bool
Disc::ReadRawSector(uint32_t lsn, std::span<std::byte> buffer) noexcept
{
	if (buffer.size() < sector_size_)
		return false;

	if (!media_->Seek(static_cast<uint64_t>(lsn) * sector_size_))
		return false;

	return media_->Read(buffer.first(sector_size_)) == sector_size_;
}

bool
Disc::ReadRawSectors(uint32_t lsn, uint32_t count, std::span<std::byte> buffer) noexcept
{
	// For PSN format, we need to read sector by sector and skip subchannel data
	if (sector_size_ == kPsnSize) {
		const std::size_t output_size = static_cast<std::size_t>(count) * kLsnSize;
		if (buffer.size() < output_size)
			return false;

		std::array<std::byte, kPsnSize> sector_buf;
		std::byte* out_ptr = buffer.data();

		for (uint32_t i = 0; i < count; ++i) {
			if (!media_->Seek((static_cast<uint64_t>(lsn) + i) * kPsnSize))
				return false;
			if (media_->Read(sector_buf) != kPsnSize)
				return false;
			// Skip 12-byte subchannel header
			std::memcpy(out_ptr, sector_buf.data() + 12, kLsnSize);
			out_ptr += kLsnSize;
		}
		return true;
	}

	// LSN format - direct read
	const std::size_t total_size = static_cast<std::size_t>(count) * kLsnSize;
	if (buffer.size() < total_size)
		return false;

	if (!media_->Seek(static_cast<uint64_t>(lsn) * kLsnSize))
		return false;

	return media_->Read(buffer.first(total_size)) == total_size;
}

} // namespace Sacd
