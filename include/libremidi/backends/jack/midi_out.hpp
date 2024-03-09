#pragma once
#include <libremidi/backends/jack/config.hpp>
#include <libremidi/backends/jack/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

#include <semaphore>

namespace libremidi
{
struct jack_queue
{
public:
  static constexpr auto size_sz = sizeof(int32_t);

  jack_queue() = default;
  jack_queue(const jack_queue&) = delete;
  jack_queue(jack_queue&&) = delete;
  jack_queue& operator=(const jack_queue&) = delete;

  jack_queue& operator=(jack_queue&& other) noexcept
  {
    ringbuffer = other.ringbuffer;
    ringbuffer_space = other.ringbuffer_space;
    other.ringbuffer = nullptr;
    return *this;
  }

  explicit jack_queue(int64_t sz) noexcept
  {
    ringbuffer = jack_ringbuffer_create(sz);
    ringbuffer_space = jack_ringbuffer_write_space(ringbuffer);
  }

  ~jack_queue() noexcept
  {
    if (ringbuffer)
      jack_ringbuffer_free(ringbuffer);
  }

  std::error_code write(const unsigned char* data, int64_t sz) const noexcept
  {
    if (static_cast<std::size_t>(sz + size_sz) > ringbuffer_space)
      return std::make_error_code(std::errc::no_buffer_space);

    while (jack_ringbuffer_write_space(ringbuffer) < sz + size_sz)
      sched_yield();

    jack_ringbuffer_write(ringbuffer, reinterpret_cast<char*>(&sz), size_sz);
    jack_ringbuffer_write(ringbuffer, reinterpret_cast<const char*>(data), sz);

    return std::error_code{};
  }

  void read(void* jack_events) const noexcept
  {
    int32_t sz;
    while (jack_ringbuffer_peek(ringbuffer, reinterpret_cast<char*>(&sz), size_sz) == size_sz
           && jack_ringbuffer_read_space(ringbuffer) >= size_sz + sz)
    {
      jack_ringbuffer_read_advance(ringbuffer, size_sz);

      if (auto midi = jack_midi_event_reserve(jack_events, 0, sz))
        jack_ringbuffer_read(ringbuffer, reinterpret_cast<char*>(midi), sz);
      else
        jack_ringbuffer_read_advance(ringbuffer, sz);
    }
  }

  jack_ringbuffer_t* ringbuffer{};
  std::size_t ringbuffer_space{}; // actual writable size, usually 1 less than ringbuffer
};

class midi_out_jack
    : public midi1::out_api
    , public jack_helpers
    , public error_handler
{
public:
  struct
      : output_configuration
      , jack_output_configuration
  {
  } configuration;

  midi_out_jack(output_configuration&& conf, jack_output_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
  }

  ~midi_out_jack() override { }

  libremidi::API get_current_api() const noexcept override { return libremidi::API::JACK_MIDI; }

  std::error_code open_port(const output_port& port, std::string_view portName) override
  {
    if (auto err = create_local_port(*this, portName, JackPortIsOutput); err != std::error_code{})
      return err;

    // Connecting to the output
    if (int err = jack_connect(this->client, jack_port_name(this->port), port.port_name.c_str()))
    {
      error(
          configuration, "JACK: could not connect to port" + port.port_name);
      return from_errc(err);
    }

    return std::error_code{};
  }

  std::error_code open_virtual_port(std::string_view portName) override
  {
    return create_local_port(*this, portName, JackPortIsOutput);
  }

  std::error_code close_port() override { return do_close_port(); }

  std::error_code set_port_name(std::string_view portName) override
  {
    int ret = jack_port_rename(this->client, this->port, portName.data());
    return from_errc(ret);
  }
};

class midi_out_jack_queued final : public midi_out_jack
{
public:
  midi_out_jack_queued(output_configuration&& conf, jack_output_configuration&& apiconf)
      : midi_out_jack{std::move(conf), std::move(apiconf)}
      , queue{configuration.ringbuffer_size}
  {
    auto status = connect(*this);
    if (status != jack_status_t{})
      warning(configuration, "midi_out_jack_queued: " + std::to_string((int)jack_status_t{}));
  }

  ~midi_out_jack_queued() override
  {
    midi_out_jack::close_port();

    disconnect(*this);
  }

  std::error_code send_message(const unsigned char* message, std::size_t size) override
  {
    return queue.write(message, size);
  }

  int process(jack_nframes_t nframes)
  {
    void* buff = jack_port_get_buffer(this->port, nframes);
    jack_midi_clear_buffer(buff);

    this->queue.read(buff);

    return 0;
  }

private:
  jack_queue queue;
};

class midi_out_jack_direct final : public midi_out_jack
{
public:
  midi_out_jack_direct(output_configuration&& conf, jack_output_configuration&& apiconf)
      : midi_out_jack{std::move(conf), std::move(apiconf)}
  {
    auto status = connect(*this);
    if (status != jack_status_t{})
      warning(configuration, "midi_out_jack_direct: " + std::to_string((int)jack_status_t{}));

    buffer_size = jack_get_buffer_size(this->client);
  }

  ~midi_out_jack_direct() override
  {
    midi_out_jack::close_port();

    disconnect(*this);
  }

  int process(jack_nframes_t nframes)
  {
    void* buff = jack_port_get_buffer(this->port, nframes);
    jack_midi_clear_buffer(buff);
    return 0;
  }

  std::error_code send_message(const unsigned char* message, size_t size) override
  {
    void* buff = jack_port_get_buffer(this->port, buffer_size);
    int ret = jack_midi_event_write(buff, 0, message, size);
    return from_errc(ret);
  }

  int convert_timestamp(int64_t user) const noexcept
  {
    switch (configuration.timestamps)
    {
      case timestamp_mode::AudioFrame:
        return static_cast<int>(user);

      default:
        // TODO
        return 0;
    }
  }

  std::error_code schedule_message(int64_t ts, const unsigned char* message, size_t size) override
  {
    void* buff = jack_port_get_buffer(this->port, buffer_size);
    int ret = jack_midi_event_write(buff, convert_timestamp(ts), message, size);
    return from_errc(ret);
  }

  int buffer_size{};
};
}

namespace libremidi
{
template <>
inline std::unique_ptr<midi_out_api> make<midi_out_jack>(
    libremidi::output_configuration&& conf, libremidi::jack_output_configuration&& api)
{
  if (api.direct)
    return std::make_unique<midi_out_jack_direct>(std::move(conf), std::move(api));
  else
    return std::make_unique<midi_out_jack_queued>(std::move(conf), std::move(api));
}
}
