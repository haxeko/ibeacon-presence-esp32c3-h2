// Request checks of the configuration web server (main/http_guard.cpp): only the SoftAP address is served (DNS
// rebinding) and every POST must be application/json (no cross-site "simple" request).
#include "test.h"

#include "http_guard.h"

static void host()
{
    CHECK(http_host_allowed("192.168.4.1", "192.168.4.1"));
    CHECK(http_host_allowed("192.168.4.1:80", "192.168.4.1"));
    CHECK(http_host_allowed(" 192.168.4.1", "192.168.4.1"));
    CHECK(!http_host_allowed("192.168.4.10", "192.168.4.1"));
    CHECK(!http_host_allowed("192.168.4.1:8080", "192.168.4.1"));
    CHECK(!http_host_allowed("192.168.4.1.attacker.example", "192.168.4.1"));
    CHECK(!http_host_allowed("attacker.example", "192.168.4.1"));
    CHECK(!http_host_allowed("192.168.4", "192.168.4.1"));
    CHECK(!http_host_allowed("", "192.168.4.1"));
    CHECK(!http_host_allowed(nullptr, "192.168.4.1"));
    CHECK(!http_host_allowed("192.168.4.1", ""));
    CHECK(!http_host_allowed("192.168.4.1", nullptr));
}

static void content_type()
{
    CHECK(http_content_type_is_json("application/json"));
    CHECK(http_content_type_is_json("application/json; charset=utf-8"));
    CHECK(http_content_type_is_json("Application/JSON;charset=UTF-8"));
    CHECK(http_content_type_is_json(" application/json"));
    // What a page of another site can send without a preflight
    CHECK(!http_content_type_is_json("text/plain"));
    CHECK(!http_content_type_is_json("text/plain;charset=UTF-8"));
    CHECK(!http_content_type_is_json("application/x-www-form-urlencoded"));
    CHECK(!http_content_type_is_json("multipart/form-data; boundary=x"));
    CHECK(!http_content_type_is_json(nullptr));
    CHECK(!http_content_type_is_json(""));
    // Look-alikes
    CHECK(!http_content_type_is_json("application/jsonp"));
    CHECK(!http_content_type_is_json("application/json-seq"));
    CHECK(!http_content_type_is_json("text/application/json"));
    CHECK(!http_content_type_is_json("application/js"));
}

int main()
{
    RUN(host);
    RUN(content_type);
    return test_summary("http_guard");
}
