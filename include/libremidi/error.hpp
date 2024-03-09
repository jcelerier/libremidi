#pragma once
#include <libremidi/config.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <libremidi/system_error2.hpp>
#pragma GCC diagnostic pop

#include <functional>
#include <string_view>

namespace libremidi
{
inline auto from_errc(int ret) noexcept
{
  return std::make_error_code(static_cast<std::errc>(ret));
}

/*! \brief Error callback function
    \param type Type of error.
    \param errorText Error description.

    Note that class behaviour is undefined after a critical error (not
    a warning) is reported.
 */
using midi_error_callback = std::function<void(std::string_view errorText)>;
using midi_warning_callback = std::function<void(std::string_view errorText)>;
}
