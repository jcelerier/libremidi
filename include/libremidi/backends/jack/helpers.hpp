#pragma once

#if __has_include(<weakjack/weak_libjack.h>)
  #include <weakjack/weak_libjack.h>
#elif __has_include(<weak_libjack.h>)
  #include <weak_libjack.h>
#elif __has_include(<jack/jack.h> )
  #include <jack/jack.h>
  #include <jack/midiport.h>
  #include <jack/ringbuffer.h>
#endif
#include <libremidi/detail/midi_in.hpp>
#include <libremidi/detail/semaphore.hpp>

#include <atomic>
#include <semaphore>

namespace libremidi
{
struct jack_client
{
  jack_client_t* client{};

  static std::string get_port_display_name(jack_port_t* port)
  {
    auto p1 = std::make_unique<char[]>(jack_port_name_size());
    auto p2 = std::make_unique<char[]>(jack_port_name_size());
    char* aliases[3] = {p1.get(), p2.get(), nullptr};
    int n = jack_port_get_aliases(port, aliases);
    if (n > 1)
    {
      return aliases[1];
    }
    else if (n > 0)
    {
      std::string str = aliases[0];
      if (str.starts_with("alsa_pcm:"))
        str.erase(0, strlen("alsa_pcm:"));
      return str;
    }
    else
    {
      const auto short_name = jack_port_short_name(port);
      if (short_name && strlen(short_name) > 0)
        return short_name;
      return jack_port_name(port);
    }
  }

  template <bool Input>
  static auto to_port_info(jack_client_t* client, jack_port_t* port)
      -> std::conditional_t<Input, input_port, output_port>
  {
    return {{
        .client = reinterpret_cast<std::uintptr_t>(client),
        .port = 0,
        .manufacturer = "",
        .device_name = "",
        .port_name = jack_port_name(port),
        .display_name = get_port_display_name(port),
    }};
  }

  template <bool Input>
  static auto
  get_ports(jack_client_t* client, const char* pattern, const JackPortFlags flags) noexcept
      -> std::vector<std::conditional_t<Input, input_port, output_port>>
  {
    std::vector<std::conditional_t<Input, input_port, output_port>> ret;

    if (!client)
      return {};

    const char** ports = jack_get_ports(client, pattern, JACK_DEFAULT_MIDI_TYPE, flags);

    if (ports == nullptr)
      return {};

    int i = 0;
    while (ports[i] != nullptr)
    {
      // FIXME this does not take into account filtering sw / hw ports
      auto port = jack_port_by_name(client, ports[i]);
      ret.push_back(to_port_info<Input>(client, port));
      i++;
    }

    jack_free(ports);

    return ret;
  }
};

struct jack_helpers : jack_client
{
  struct port_handle
  {
    port_handle& operator=(jack_port_t* p)
    {
      impl.get()->store(p);
      return *this;
    }

    operator jack_port_t*() const noexcept
    {
      if (impl)
        return impl.get()->load();
      return {};
    }

    std::shared_ptr<std::atomic<jack_port_t*>> impl
        = std::make_shared<std::atomic<jack_port_t*>>(nullptr);
  } port;

  int64_t this_instance{};

  semaphore_pair_lock thread_lock;

  jack_helpers()
  {
    static std::atomic_int64_t instance{};
    this_instance = ++instance;
  }

  template <typename Self>
  jack_status_t connect(Self& self)
  {
    auto& configuration = self.configuration;

    if (this->client)
      return jack_status_t{};

    // Initialize JACK client
    if (configuration.context)
    {
      if (!configuration.set_process_func)
        return JackFailure;

      configuration.set_process_func(
          {.token = this_instance,
           .callback = [&self, p = std::weak_ptr{this->port.impl}](jack_nframes_t nf) -> int {
             if (auto pt = p.lock())
               if (auto ppt = pt->load())
                 self.process(nf);

             self.thread_lock.check_client_released();
             return 0;
           }});

      this->client = configuration.context;
      return jack_status_t{};
    }
    else
    {
      jack_status_t status{};
      this->client
          = jack_client_open(configuration.client_name.c_str(), JackNoStartServer, &status);
      if (this->client != nullptr)
      {
        jack_set_process_callback(
            this->client,
            +[](jack_nframes_t nf, void* ctx) -> int {
              auto& self = *static_cast<Self*>(ctx);
              jack_port_t* port = self.port;

              // Is port created?
              if (port == nullptr)
                return 0;

              self.process(nf);

              self.thread_lock.check_client_released();
              return 0;
            },
            &self);
        jack_activate(this->client);
      }
      return status;
    }
  }

  template <typename Self>
  void disconnect(Self& self)
  {
    if (self.configuration.context)
    {
      if (self.configuration.clear_process_func)
      {
        self.configuration.clear_process_func(this_instance);
      }
    }

    if (this->client && !self.configuration.context)
      jack_client_close(this->client);
  }

  stdx::error
  create_local_port(const auto& self, std::string_view portName, JackPortFlags flags)
  {
    // full name: "client_name:port_name\0"
    if (portName.empty())
      portName = flags & JackPortIsInput ? "i" : "o";

    if (self.configuration.client_name.size() + portName.size() + 2u
        >= static_cast<size_t>(jack_port_name_size()))
    {
      self.template error(
          self.configuration, "JACK: port name length limit exceeded");
      return std::make_error_code(std::errc::invalid_argument);
    }

    if (!this->port)
    {
      this->port
          = jack_port_register(this->client, portName.data(), JACK_DEFAULT_MIDI_TYPE, flags, 0);
    }

    if (!this->port)
    {
      self.template error(self.configuration, "JACK: error creating port");
      return std::make_error_code(std::errc::operation_not_supported);
    }
    return stdx::error{};
  }

  stdx::error do_close_port()
  {
    if (this->port == nullptr)
      return stdx::error{};

    // 1. Ensure that the next time the cycle runs it sees the port as nullptr
    jack_port_t* port_ptr = this->port.impl->load();
    this->port = nullptr;

    // 2. Signal through the semaphore and wait for the signal return
    this->thread_lock.prepare_release_client();

    // 3. Now we are sure that the client is not going to use the port anymore
    int err = jack_port_unregister(this->client, port_ptr);
    return from_errc(err);
  }
};
}
