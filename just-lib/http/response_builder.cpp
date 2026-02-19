#include "response_builder.hpp"

#include <cstdlib>

namespace just::http {

char* ResponseBuilder::success(json_t* data_object) {
    if (!data_object) {
        return nullptr;
    }

    json_t* root = json_object();
    if (!root) {
        return nullptr;
    }

    if (json_object_set_new(root, "success", json_true()) != 0) {
        json_decref(root);
        return nullptr;
    }

    if (json_object_set_new(root, "data", data_object) != 0) {
        json_decref(root);
        return nullptr;
    }

    char* json_str = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    json_decref(root);
    return json_str;
}

char* ResponseBuilder::error(int code, const char* error_type, const char* message) {
    if (!error_type || !message) {
        return nullptr;
    }

    json_t* root = json_object();
    if (!root) {
        return nullptr;
    }

    if (json_object_set_new(root, "success", json_false()) != 0) {
        json_decref(root);
        return nullptr;
    }

    json_t* error_obj = json_object();
    if (!error_obj) {
        json_decref(root);
        return nullptr;
    }

    if (json_object_set_new(error_obj, "code", json_integer(code)) != 0 ||
        json_object_set_new(error_obj, "type", json_string(error_type)) != 0 ||
        json_object_set_new(error_obj, "message", json_string(message)) != 0) {
        json_decref(error_obj);
        json_decref(root);
        return nullptr;
    }

    if (json_object_set_new(root, "error", error_obj) != 0) {
        json_decref(error_obj);
        json_decref(root);
        return nullptr;
    }

    char* json_str = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    json_decref(root);
    return json_str;
}

const char* ResponseBuilder::errorType(int code) noexcept {
    switch (code) {
        case HTTP_BAD_REQUEST:
            return "Bad Request";
        case HTTP_NOT_FOUND:
            return "Not Found";
        case HTTP_INTERNAL_ERROR:
            return "Internal Server Error";
        case HTTP_OK:
            return "OK";
        default:
            return "Unknown Error";
    }
}

std::string ResponseBuilder::successString(json_t* data_object) {
    char* raw = success(data_object);
    if (!raw) {
        return {};
    }
    std::string out(raw);
    std::free(raw);
    return out;
}

std::string ResponseBuilder::errorString(int code, const char* error_type, const char* message) {
    char* raw = error(code, error_type, message);
    if (!raw) {
        return {};
    }
    std::string out(raw);
    std::free(raw);
    return out;
}

} // namespace just::http

extern "C" {

char* response_builder_success_cpp(json_t* data_object) {
    return just::http::ResponseBuilder::success(data_object);
}

char* response_builder_error_cpp(int code, const char* error_type, const char* message) {
    return just::http::ResponseBuilder::error(code, error_type, message);
}

const char* response_builder_get_error_type_cpp(int code) {
    return just::http::ResponseBuilder::errorType(code);
}

} // extern "C"