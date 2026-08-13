// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "DstDecoder.hxx"
#include "Domain.hxx"
#include "Log.hxx"
#include "libdstdec/binding/dst_decoder.hxx"

#include <cstring>

namespace Sacd {

/**
 * DST frames smaller than this cannot hold valid data.
 */
static constexpr std::size_t MIN_DST_FRAME_SIZE = 20;

/**
 * The DSD sample value that represents silence.
 */
static constexpr int DSD_SILENCE = 0xAA;

static constexpr unsigned kDsd64FrameBytes = 4704;

class DstDecoder::Impl {
public:
	Dst::DecoderBinding decoder;
	unsigned channel_count = 0;
	unsigned frame_size = 0;
	bool initialized = false;
};

DstDecoder::DstDecoder() noexcept
	: pimpl_(std::make_unique<Impl>())
{
}

DstDecoder::~DstDecoder() noexcept = default;

bool
DstDecoder::Initialize(unsigned channel_count, unsigned sample_rate) noexcept
{

	if (channel_count == 0 || channel_count > 6)
		return false;

	unsigned frame_bytes;
	switch (sample_rate) {
	case 2822400:
		frame_bytes = kDsd64FrameBytes;
		break;
	case 5644800:
		frame_bytes = kDsd64FrameBytes * 2;
		break;
	case 11289600:
		frame_bytes = kDsd64FrameBytes * 4;
		break;
	default:
		frame_bytes = kDsd64FrameBytes;
		break;
	}

	pimpl_->channel_count = channel_count;
	pimpl_->frame_size = frame_bytes * channel_count;


	int result = pimpl_->decoder.Initialize(channel_count, frame_bytes);
	if (result != 0) {
		LogDebug(sacdiso_domain, "DST decoder.Initialize failed");
		return false;
	}

	pimpl_->initialized = true;
	return true;
}

bool
DstDecoder::IsInitialized() const noexcept
{
	return pimpl_ && pimpl_->initialized;
}

bool
DstDecoder::Decode(std::span<const std::byte> dst_data,
                   std::vector<std::byte> &dsd_output) noexcept
{
	if (!pimpl_->initialized)
		return false;

	if (dst_data.empty()) {
		dsd_output.clear();
		return true;
	}

	/* frames smaller than this cannot contain valid DST data; emit
	   DSD silence instead.  0xAA matches the decoder's initial channel
	   status - using 0x55 would cause a discontinuity once real
	   decoding starts. */
	if (dst_data.size() < MIN_DST_FRAME_SIZE) {
		dsd_output.resize(pimpl_->frame_size);
		std::memset(dsd_output.data(), DSD_SILENCE, dsd_output.size());
		return true;
	}

	std::vector<uint8_t> buffer(
		reinterpret_cast<const uint8_t *>(dst_data.data()),
		reinterpret_cast<const uint8_t *>(dst_data.data()) + dst_data.size()
	);

	int result = pimpl_->decoder.Decode(buffer);

	if (result <= 0) {
		/* decode failed or returned nothing - emit silence */
		dsd_output.resize(pimpl_->frame_size);
		std::memset(dsd_output.data(), DSD_SILENCE, dsd_output.size());
		return true;
	}

	dsd_output.resize(static_cast<std::size_t>(result));
	std::memcpy(dsd_output.data(), buffer.data(), static_cast<std::size_t>(result));
	return true;
}

std::size_t
DstDecoder::GetOutputFrameSize() const noexcept
{
	return pimpl_->initialized ? pimpl_->frame_size : 0;
}

void
DstDecoder::Reset() noexcept
{
	if (pimpl_->initialized) {
		pimpl_->decoder.Flush();
	}
}

} // namespace Sacd
