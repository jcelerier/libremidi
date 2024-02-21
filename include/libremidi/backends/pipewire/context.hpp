#pragma once
#include <libremidi/backends/linux/pipewire.hpp>
#include <libremidi/detail/memory.hpp>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/param/props.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <spa/param/audio/format-utils.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
namespace libremidi
{
template <typename K, typename V>
using hash_map = std::unordered_map<K, V>;

struct pipewire_instance
{
  const libpipewire& pw = libpipewire::instance();
  pipewire_instance()
  {
    /// Initialize the PipeWire main loop, context, etc.
    int argc = 0;
    char* argv[] = {NULL};
    char** aa = argv;
    pw.init(&argc, &aa);
  }

  ~pipewire_instance() { pw.deinit(); }
};

struct pipewire_context
{
  const libpipewire& pw = libpipewire::instance();
  std::shared_ptr<pipewire_instance> global_instance;

  pw_main_loop* main_loop{};
  pw_loop* lp{};

  pw_context* context{};
  pw_core* core{};

  pw_registry* registry{};
  spa_hook registry_listener{};

  struct listened_port
  {
    uint32_t id{};
    pw_port* port{};
    std::unique_ptr<spa_hook> listener;
  };
  std::vector<listened_port> port_listener{};

  struct port_info
  {
    uint32_t id{};

    std::string format;
    std::string port_name;
    std::string port_alias;
    std::string object_path;
    std::string node_id;
    std::string port_id;

    bool physical{};
    bool terminal{};
    bool monitor{};
    pw_direction direction{};
  };

  struct node
  {
    std::vector<port_info> inputs;
    std::vector<port_info> outputs;
  };

  struct graph
  {
    libremidi::hash_map<uint32_t, node> physical_audio;
    libremidi::hash_map<uint32_t, node> physical_midi;
    libremidi::hash_map<uint32_t, node> software_audio;
    libremidi::hash_map<uint32_t, node> software_midi;

    void for_each_port(auto func)
    {
      for (auto& map : {physical_audio, physical_midi, software_audio, software_midi})
      {
        for (auto& [id, node] : map)
        {
          for (auto& port : node.inputs)
            func(port);
          for (auto& port : node.outputs)
            func(port);
        }
      }
    }

    void remove_port(uint32_t id)
    {
      for (auto map : {&physical_audio, &physical_midi, &software_audio, &software_midi})
      {
        for (auto& [_, node] : *map)
        {
          std::erase_if(node.inputs, [id](const port_info& p) { return p.id == id; });
          std::erase_if(node.outputs, [id](const port_info& p) { return p.id == id; });
        }
      }
    }
  } current_graph;

  int sync{};

  explicit pipewire_context(std::shared_ptr<pipewire_instance> inst)
      : global_instance{inst}
  {
    this->main_loop = pw.main_loop_new(nullptr);
    if (!this->main_loop)
    {
      // libremidi::logger().error("PipeWire: main_loop_new failed!");
      return;
    }

    this->lp = pw.main_loop_get_loop(this->main_loop);
    if (!lp)
    {
      // libremidi::logger().error("PipeWire: main_loop_get_loop failed!");
      return;
    }

    this->context = pw.context_new(lp, nullptr, 0);
    if (!this->context)
    {
      // libremidi::logger().error("PipeWire: context_new failed!");
      return;
    }

    this->core = pw.context_connect(this->context, nullptr, 0);
    if (!this->core)
    {
      // libremidi::logger().error("PipeWire: context_connect failed!");
      return;
    }

    this->registry = pw_core_get_registry(this->core, PW_VERSION_REGISTRY, 0);
    if (!this->registry)
    {
      // libremidi::logger().error("PipeWire: core_get_registry failed!");
      return;
    }

    // Register a listener which will listen on when ports are added / removed
    spa_zero(registry_listener);
    static constexpr const struct pw_port_events port_events = {
        .version = PW_VERSION_PORT_EVENTS,
        .info = [](void* object,
                   const pw_port_info* info) { ((pipewire_context*)object)->register_port(info); },
    };

    static constexpr const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global =
            [](void* object, uint32_t id, uint32_t /*permissions*/, const char* type,
               uint32_t /*version*/, const struct spa_dict* /*props*/) {
      pipewire_context& self = *(pipewire_context*)object;

      // When a port is added:
      if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
      {
        auto port = (pw_port*)pw_registry_bind(self.registry, id, type, PW_VERSION_PORT, 0);
        self.port_listener.push_back({id, port, std::make_unique<spa_hook>()});
        auto& l = self.port_listener.back();

        pw_port_add_listener(l.port, l.listener.get(), &port_events, &self);
      }
        },
        .global_remove =
            [](void* object, uint32_t id) {
      pipewire_context& self = *(pipewire_context*)object;

      // When a port is removed:
      // Remove from the graph
      self.current_graph.remove_port(id);

      // Remove from the listeners
      auto it = std::find_if(
          self.port_listener.begin(), self.port_listener.end(),
          [&](const listened_port& l) { return l.id == id; });
      if (it != self.port_listener.end())
      {
        libpipewire::instance().proxy_destroy((pw_proxy*)it->port);
        self.port_listener.erase(it);
      }
        },
        };

    // Start listening
    pw_registry_add_listener(this->registry, &this->registry_listener, &registry_events, this);

    synchronize();

    // Add a manual 1ms event loop iteration at the end of
    // ctor to ensure synchronous clients will still see the ports
    pw_loop_iterate(this->lp, 1);
  }

  std::atomic<int> pending{};
  std::atomic<int> done{};
  void synchronize()
  {
    pending = 0;
    done = 0;

    if (!core)
      return;

    spa_hook core_listener;

    static constexpr struct pw_core_events core_events = {
        .version = PW_VERSION_CORE_EVENTS,
        .done =
        [](void* object, uint32_t id, int seq) {
      auto& self = *(pipewire_context*)object;
      if(id == PW_ID_CORE && seq == self.pending)
      {
        self.done = 1;
        libpipewire::instance().main_loop_quit(self.main_loop);
      }
        },
    };

    spa_zero(core_listener);
    pw_core_add_listener(core, &core_listener, &core_events, this);

    pending = pw_core_sync(core, PW_ID_CORE, 0);
    while (!done)
    {
      pw.main_loop_run(this->main_loop);
    }
    spa_hook_remove(&core_listener);
  }

  pw_proxy* link_ports(uint32_t out_port, uint32_t in_port)
  {
    auto props = pw.properties_new(
        PW_KEY_LINK_OUTPUT_PORT, std::to_string(out_port).c_str(), PW_KEY_LINK_INPUT_PORT,
        std::to_string(in_port).c_str(), nullptr);

    auto proxy = (pw_proxy*)pw_core_create_object(
        this->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);

    if (!proxy)
    {
      std::cerr << "PipeWire: could not allocate link\n";
      pw.properties_free(props);
      return nullptr;
    }

    synchronize();
    pw.properties_free(props);
    return proxy;
  }

  void unlink_ports(pw_proxy* link) { pw.proxy_destroy(link); }

  void register_port(const pw_port_info* info)
  {
    const spa_dict_item* item{};

    port_info p;
    p.id = info->id;

    spa_dict_for_each(item, info->props)
    {
      std::string_view k{item->key}, v{item->value};
      if (k == "format.dsp")
        p.format = v;
      else if (k == "port.name")
        p.port_name = v;
      else if (k == "port.alias")
        p.port_alias = v;
      else if (k == "object.path")
        p.object_path = v;
      else if (k == "port.id")
        p.port_id = v;
      else if (k == "node.id")
        p.node_id = v;
      else if (k == "port.physical" && v == "true")
        p.physical = true;
      else if (k == "port.terminal" && v == "true")
        p.terminal = true;
      else if (k == "port.monitor" && v == "true")
        p.monitor = true;
      else if (k == "port.direction")
      {
        if (v == "out")
        {
          p.direction = pw_direction::SPA_DIRECTION_OUTPUT;
        }
        else
        {
          p.direction = pw_direction::SPA_DIRECTION_INPUT;
        }
      }
    }

    if (p.node_id.empty())
      return;

    const auto nid = std::stoul(p.node_id);
    if (p.physical)
    {
      if (p.format.find("audio") != p.format.npos)
      {
        if (p.direction == pw_direction::SPA_DIRECTION_OUTPUT)
          this->current_graph.physical_audio[nid].outputs.push_back(std::move(p));
        else
          this->current_graph.physical_audio[nid].inputs.push_back(std::move(p));
      }
      else if (p.format.find("midi") != p.format.npos)
      {
        if (p.direction == pw_direction::SPA_DIRECTION_OUTPUT)
          this->current_graph.physical_midi[nid].outputs.push_back(std::move(p));
        else
          this->current_graph.physical_midi[nid].inputs.push_back(std::move(p));
      }
      else
      {
        // TODO, video ?
      }
    }
    else
    {
      if (p.format.find("audio") != p.format.npos)
      {
        if (p.direction == pw_direction::SPA_DIRECTION_OUTPUT)
          this->current_graph.software_audio[nid].outputs.push_back(std::move(p));
        else
          this->current_graph.software_audio[nid].inputs.push_back(std::move(p));
      }
      else if (p.format.find("midi") != p.format.npos)
      {
        if (p.direction == pw_direction::SPA_DIRECTION_OUTPUT)
          this->current_graph.software_midi[nid].outputs.push_back(std::move(p));
        else
          this->current_graph.software_midi[nid].inputs.push_back(std::move(p));
      }
      else
      {
        // TODO, video ?
      }
    }
  }

  int get_fd() const noexcept
  {
    if (!this->lp)
      return -1;

    auto spa_callbacks = this->lp->control->iface.cb;
    auto spa_loop_methods = (const spa_loop_control_methods*)spa_callbacks.funcs;
    if (spa_loop_methods->get_fd)
      return spa_loop_methods->get_fd(spa_callbacks.data);
    else
      return -1;
  }

  ~pipewire_context()
  {
    if (this->registry)
      pw.proxy_destroy((pw_proxy*)this->registry);
    for (auto& [id, p, l] : this->port_listener)
      if (l)
        pw.proxy_destroy((pw_proxy*)p);
    if (this->core)
      pw.core_disconnect(this->core);
    if (this->context)
      pw.context_destroy(this->context);
    if (this->main_loop)
      pw.main_loop_destroy(this->main_loop);
  }
};

struct pipewire_filter
{
  const libpipewire& pw = libpipewire::instance();
  std::shared_ptr<pipewire_context> loop{};
  pw_filter* filter{};
  std::vector<pw_proxy*> links{};

  struct port
  {
    void* data;
  }* port{};

  explicit pipewire_filter(std::shared_ptr<pipewire_context> loop)
      : loop{loop}
  {
  }

  void create_filter(std::string_view filter_name, const pw_filter_events& events, void* context)
  {
    auto& pw = libpipewire::instance();
    // clang-format off
    this->filter = pw.filter_new_simple(
        loop->lp,
        filter_name.data(), // FIXME
        pw.properties_new(
            PW_KEY_MEDIA_TYPE, "Midi",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_MEDIA_NAME, "libremidi",
            PW_KEY_NODE_LOCK_RATE, "true",
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            PW_KEY_NODE_PAUSE_ON_IDLE, "false",
            PW_KEY_NODE_SUSPEND_ON_IDLE, "false",
            nullptr),
        &events,
        context);
    // clang-format on
    assert(filter);
  }

  void create_local_port(std::string_view port_name, spa_direction direction)
  {
    // clang-format off
    this->port = (struct port*)pw.filter_add_port(
        this->filter,
        direction,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(struct port),
        pw.properties_new(
            PW_KEY_FORMAT_DSP, "8 bit raw midi",
            PW_KEY_PORT_NAME, port_name.data(),
            nullptr),
        nullptr, 0);
    // clang-format on
    assert(port);
  }

  void remove_port()
  {
    assert(port);
    pw.filter_remove_port(this->port);
    this->port = nullptr;
  }

  void rename_port(std::string_view port_name)
  {
    assert(port);
    spa_dict_item items[1] = {
        SPA_DICT_ITEM_INIT(PW_KEY_PORT_NAME, port_name.data()),
    };

    auto properties = SPA_DICT_INIT(items, 1);
    pw.filter_update_properties(this->filter, this->port, &properties);
    this->port = nullptr;
  }

  void start_filter()
  {
    if (pw.filter_connect(this->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0)
    {
      std::cerr << "can't connect\n";
      return;
    }
  }

  uint32_t filter_node_id() { return this->loop->pw.filter_get_node_id(this->filter); }

  void synchronize_node()
  {
    this->loop->synchronize();
    int k = 0;
    auto node_id = filter_node_id();
    while (node_id == 4294967295)
    {
      this->loop->synchronize();
      node_id = filter_node_id();

      if (k++; k > 100)
        return;
    }
  }
  void synchronize_ports(const pipewire_context::node& this_node)
  {
    // Leave some time to resolve the ports
    int k = 0;
    const auto num_local_ins = 1;
    const auto num_local_outs = 0;
    while (this_node.inputs.size() < num_local_ins || this_node.outputs.size() < num_local_outs)
    {
      this->loop->synchronize();
      if (k++; k > 100)
        return;
    }
  }
};
}

#pragma GCC diagnostic pop
