#pragma once

#include <jansson.h>
#include <string>

namespace just::http {

class ResponseBuilder {
public:
    static constexpr int HTTP_OK = 200;
    static constexpr int HTTP_BAD_REQUEST = 400;
    static constexpr int HTTP_NOT_FOUND = 404;
    static constexpr int HTTP_INTERNAL_ERROR = 500;

    static char* success(json_t* data_object);
    static char* error(int code, const char* error_type, const char* message);
    static const char* errorType(int code) noexcept;

    static std::string successString(json_t* data_object);
    static std::string errorString(int code, const char* error_type, const char* message);
};

} // namespace just::http

extern "C" {
char* response_builder_success_cpp(json_t* data_object);
char* response_builder_error_cpp(int code, const char* error_type, const char* message);
const char* response_builder_get_error_type_cpp(int code);
}