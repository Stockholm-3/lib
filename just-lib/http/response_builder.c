#include "response_builder.h"

char* response_builder_success_cpp(json_t* data_object);
char* response_builder_error_cpp(int code, const char* error_type,
                                 const char* message);
const char* response_builder_get_error_type_cpp(int code);

char* response_builder_success(json_t* data_object) {
    return response_builder_success_cpp(data_object);
}

char* response_builder_error(int code, const char* error_type,
                             const char* message) {
    return response_builder_error_cpp(code, error_type, message);
}

const char* response_builder_get_error_type(int code) {
    return response_builder_get_error_type_cpp(code);
}
