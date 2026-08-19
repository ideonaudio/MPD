/*
 * Unit tests for src/util/
 */

#include "uri/Extract.hxx"

#include <gtest/gtest.h>

using std::string_view_literals::operator""sv;

static constexpr struct UriTests {
	const char *uri;
	const char *host_and_port;
	const char *path;
	const char *query_string;
} uri_tests[] = {
	{ "http://foo/bar", "foo", "/bar", nullptr },
	{ "https://foo/bar", "foo", "/bar", nullptr },
	{ "http://foo:8080/bar", "foo:8080", "/bar", nullptr },
	{ "http://foo", "foo", nullptr, nullptr },
	{ "http://foo/bar?a=b", "foo", "/bar?a=b", "a=b" },
	{ "whatever-scheme://foo/bar?a=b", "foo", "/bar?a=b", "a=b" },
	{ "//foo/bar", "foo", "/bar", nullptr },
	{ "//foo", "foo", nullptr, nullptr },
	{ "/bar?a=b", nullptr, "/bar?a=b", "a=b" },
	{ "bar?a=b", nullptr, "bar?a=b", "a=b" },
};

TEST(UriExtractTest, Path)
{
	for (auto i : uri_tests) {
		auto result = UriPathQueryFragment(i.uri);
		if (i.path == nullptr)
			ASSERT_EQ(result.data(), nullptr);
		else
			ASSERT_EQ(result, i.path);
	}
}

TEST(UriExtract, Suffix)
{
	EXPECT_EQ((const char *)nullptr, uri_get_suffix("/foo/bar").data());
	EXPECT_EQ((const char *)nullptr, uri_get_suffix("/foo.jpg/bar").data());
	EXPECT_EQ(uri_get_suffix("/foo/bar.jpg"), "jpg"sv);
	EXPECT_EQ(uri_get_suffix("/foo.png/bar.jpg"), "jpg"sv);
	EXPECT_EQ((const char *)nullptr, uri_get_suffix(".jpg").data());
	EXPECT_EQ((const char *)nullptr, uri_get_suffix("/foo/.jpg").data());

	/* eliminate the query string */
	EXPECT_EQ(uri_get_suffix("/foo/bar.jpg?query_string"), "jpg"sv);
}
