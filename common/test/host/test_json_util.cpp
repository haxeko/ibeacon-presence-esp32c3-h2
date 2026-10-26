// JSON escaping and UTF-8 checks (main/json_util.cpp): names from the air and labels from the page cannot
// break the JSON or inject markup.
#include "test.h"

#include "json_util.h"

static void escape_basics()
{
    char out[128];
    json_escape("plain text", out, sizeof(out));
    CHECK_STR(out, "plain text");
    json_escape("a\"b\\c", out, sizeof(out));
    CHECK_STR(out, "a\\\"b\\\\c");
    json_escape("line\nnext\ttab\x01", out, sizeof(out));
    CHECK_STR(out, "line\\nnext\\u0009tab\\u0001");
}

static void escape_markup()
{
    char out[128];
    json_escape("<script>alert('x')&</script>", out, sizeof(out));
    CHECK_STR(out, "\\u003cscript\\u003ealert(\\u0027x\\u0027)\\u0026\\u003c/script\\u003e");
    CHECK(strchr(out, '<') == nullptr && strchr(out, '>') == nullptr);
}

static void escape_utf8()
{
    char out[64];
    json_escape("Phòng ngủ", out, sizeof(out)); // valid UTF-8 kept as is
    CHECK_STR(out, "Phòng ngủ");
    json_escape("bad\xFF\xC3", out, sizeof(out)); // invalid bytes become '?'
    CHECK_STR(out, "bad??");
}

static void escape_truncation()
{
    char out[8];
    // "ab" + < (6 chars) does not fit in 7 characters + NUL: stop before the escape, never in the middle
    CHECK(json_escape("ab<c", out, sizeof(out)) == 2);
    CHECK_STR(out, "ab");
    char out2[5];
    json_escape("xxx\xE1\xBA\xA1", out2, sizeof(out2)); // 3-byte letter does not fit after "xxx"
    CHECK_STR(out2, "xxx");
    char one[1];
    CHECK(json_escape("abc", one, 1) == 0 && one[0] == '\0');
}

static void utf8_checks()
{
    CHECK(utf8_is_valid_text("Hello", 5));
    CHECK(utf8_is_valid_text("Bếp", strlen("Bếp")));
    CHECK(!utf8_is_valid_text("a\x7F", 2));
    CHECK(!utf8_is_valid_text("\xC0\xAF", 2));         // overlong '/'
    CHECK(!utf8_is_valid_text("\xED\xA0\x80", 3));     // surrogate
    CHECK(!utf8_is_valid_text("\xF4\x90\x80\x80", 4)); // above U+10FFFF
    CHECK(!utf8_is_valid_text("\xE1\xBA", 2));         // truncated
    CHECK(utf8_is_valid_text("\xF0\x9F\x98\x80", 4));  // emoji
    CHECK(utf8_is_valid_text("", 0));
}

static void sanitize()
{
    char out[16];
    const uint8_t in[] = { 'o', 'k', 0x00, 0x1F, 0xE1, 0xBA, 0xA1, 0x80 };
    CHECK(utf8_sanitize(in, sizeof(in), out, sizeof(out)) == 8);
    CHECK_STR(out, "ok??\xE1\xBA\xA1?");
}

int main()
{
    RUN(escape_basics);
    RUN(escape_markup);
    RUN(escape_utf8);
    RUN(escape_truncation);
    RUN(utf8_checks);
    RUN(sanitize);
    return test_summary("json_util");
}
