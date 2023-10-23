
#include "server/ClientPort.h"
#include "GrabbedDevices.h"
#include "VirtualDevice.h"
#include "server/ButtonDebouncer.h"
#include "server/Settings.h"
#include "server/verbose_debug_io.h"
#include "runtime/Stage.h"
#include "runtime/Timeout.h"
#include "common/output.h"

namespace {
  const auto virtual_device_name = "Keymapper";
  const auto no_device_index = 10000;

  ClientPort g_client;
  std::unique_ptr<Stage> g_stage;
  VirtualDevice g_virtual_device;
  GrabbedDevices g_grabbed_devices;
  std::optional<ButtonDebouncer> g_button_debouncer;
  std::vector<KeyEvent> g_send_buffer;
  std::optional<Clock::time_point> g_flush_scheduled_at;
  std::optional<Clock::time_point> g_input_timeout_start;
  std::chrono::milliseconds g_input_timeout;
  std::vector<Key> g_virtual_keys_down;
  KeyEvent g_last_key_event;
  int g_last_device_index;

  void translate_input(const KeyEvent& input, int device_index);

  void evaluate_device_filters() {
    g_stage->evaluate_device_filters(g_grabbed_devices.grabbed_device_names());
  }

  bool read_client_messages(std::optional<Duration> timeout = { }) {
    return g_client.read_messages(timeout, [&](Deserializer& d) {
      const auto message_type = d.read<MessageType>();
      if (message_type == MessageType::configuration) {
        const auto prev_stage = std::move(g_stage);
        g_stage = g_client.read_config(d);
        verbose("Received configuration");

        if (prev_stage &&
            prev_stage->has_mouse_mappings() != g_stage->has_mouse_mappings()) {
          verbose("Mouse usage in configuration changed");
          g_stage.reset();
        }
        else {
          evaluate_device_filters();
        }
      }
      else if (message_type == MessageType::active_contexts) {
        const auto& contexts = g_client.read_active_contexts(d);
        verbose("Received contexts (%d)", contexts.size());
        if (g_stage)
          g_stage->set_active_contexts(contexts);
      }
    });
  }

  bool read_initial_config() {
    while (!g_stage) {
      if (!read_client_messages()) {
        error("Receiving configuration failed");
        return false;
      }
    }
    return true;
  }

  void schedule_flush(Duration delay) {
    if (g_flush_scheduled_at)
      return;
    g_flush_scheduled_at = Clock::now() +
      std::chrono::duration_cast<Clock::duration>(delay);
  }

  void toggle_virtual_key(Key key) {
    const auto it = std::find(g_virtual_keys_down.begin(), g_virtual_keys_down.end(), key);
    if (it == g_virtual_keys_down.end()) {
      g_virtual_keys_down.push_back(key);
      translate_input({ key, KeyState::Down }, no_device_index);
    }
    else {
      g_virtual_keys_down.erase(it);
      translate_input({ key, KeyState::Up }, no_device_index);
    }
  }

  bool flush_send_buffer() {
    auto i = 0;
    for (; i < g_send_buffer.size(); ++i) {
      const auto& event = g_send_buffer[i];
      const auto is_last = (i == g_send_buffer.size() - 1);

      if (is_action_key(event.key)) {
        if (event.state == KeyState::Down)
          g_client.send_triggered_action(
            static_cast<int>(*event.key - *Key::first_action));
        continue;
      }

      if (is_virtual_key(event.key)) {
        if (event.state == KeyState::Down)
          toggle_virtual_key(event.key);
        continue;
      }

      if (event.key == Key::timeout) {
        schedule_flush(timeout_to_milliseconds(event.timeout));
        ++i;
        break;
      }

      if (g_button_debouncer &&
          event.state == KeyState::Down) {
        const auto delay = g_button_debouncer->on_key_down(event.key, !is_last);
        if (delay.count() > 0) {
          schedule_flush(delay);
          break;
        }
      }
      if (!g_virtual_device.send_key_event(event))
        return false;
    }
    g_send_buffer.erase(g_send_buffer.begin(), g_send_buffer.begin() + i);
    
    return g_virtual_device.flush();
  }

  void send_key_sequence(const KeySequence& sequence) {
    g_send_buffer.insert(g_send_buffer.end(), sequence.begin(), sequence.end());
  }

  void translate_input(const KeyEvent& input, int device_index) {
    // ignore key repeat while a flush or a timeout is pending
    if (input == g_last_key_event &&
        (g_flush_scheduled_at || g_input_timeout_start))
      return;

    // cancel timeout when key is released/another is pressed
    if (g_input_timeout_start) {
      const auto time_since_timeout_start = 
        (Clock::now() - *g_input_timeout_start);
      g_input_timeout_start.reset();
      translate_input(make_input_timeout_event(time_since_timeout_start),
        device_index);
    }

    g_last_key_event = input;
    g_last_device_index = device_index;

    auto output = g_stage->update(input, device_index);

    verbose_debug_io(input, output, true);

    // waiting for input timeout
    if (!output.empty() && is_input_timeout_event(output.back())) {
      g_input_timeout_start = Clock::now();
      g_input_timeout = timeout_to_milliseconds(output.back().timeout);
      output.pop_back();
    }

    send_key_sequence(output);

    g_stage->reuse_buffer(std::move(output));
  }

  bool main_loop() {
    for (;;) {
      // wait for next input event
      auto now = Clock::now();
      auto timeout = std::optional<Duration>();
      const auto set_timeout = [&](const Duration& duration) {
        if (!timeout || duration < timeout)
          timeout = duration;
      };
      if (g_flush_scheduled_at)
        set_timeout(g_flush_scheduled_at.value() - now);
      if (g_input_timeout_start)
        set_timeout(g_input_timeout_start.value() + g_input_timeout - now);

      // interrupt waiting when no key is down and client sends an update
      const auto interrupt_fd = (!g_stage->is_output_down() ? g_client.socket() : -1);

      const auto [succeeded, input] =
        g_grabbed_devices.read_input_event(timeout, interrupt_fd);
      if (!succeeded) {
        error("Reading input event failed");
        return true;
      }

      now = Clock::now();

      if (input) {
        if (auto event = to_key_event(input.value())) {
          translate_input(event.value(), input->device_index);
        }
        else {
          // forward other events
          g_virtual_device.send_event(input->type, input->code, input->value);
          continue;
        }
      }

      if (g_input_timeout_start &&
          now >= g_input_timeout_start.value() + g_input_timeout) {
        g_input_timeout_start.reset();
        translate_input(make_input_timeout_event(g_input_timeout),
          g_last_device_index);
      }

      if (!g_flush_scheduled_at || now > g_flush_scheduled_at) {
        g_flush_scheduled_at.reset();
        if (!flush_send_buffer()) {
          error("Sending input failed");
          return true;
        }
      }

      // let client update configuration and context
      if (interrupt_fd >= 0)
        if (!read_client_messages(Duration::zero()) ||
            !g_stage) {
          verbose("Connection to keymapper reset");
          return true;
        }

      if (g_stage->should_exit()) {
        verbose("Read exit sequence");
        return false;
      }
    }
  }

  int connection_loop() {
    for (;;) {
      verbose("Waiting for keymapper to connect");
      if (!g_client.accept()) {
        error("Accepting client connection failed");
        continue;
      }

      if (read_initial_config()) {
        verbose("Creating virtual device '%s'", virtual_device_name);
        if (!g_virtual_device.create(virtual_device_name)) {
          error("Creating virtual device failed");
          return 1;
        }

        if (!g_grabbed_devices.grab(virtual_device_name,
              g_stage->has_mouse_mappings())) {
          error("Initializing input device grabbing failed");
          g_virtual_device = { };
          return 1;
        }

        evaluate_device_filters();

        verbose("Entering update loop");
        if (!main_loop()) {
          verbose("Exiting");
          return 0;
        }
      }
      g_grabbed_devices = { };
      g_virtual_device = { };
      g_client.disconnect();
      verbose("---------------");
    }
  }
} // namespace

int main(int argc, char* argv[]) {
  auto settings = Settings{ };

  if (!interpret_commandline(settings, argc, argv)) {
    print_help_message();
    return 1;
  }
  g_verbose_output = settings.verbose;
  if (settings.debounce)
    g_button_debouncer.emplace();

#if defined(__APPLE__)
  // when running as user in the graphical environment try to grab input device and exit.
  // it will fail but user is asked to grant permanent permission to monitor input.
  if (settings.grab_and_exit)
    return g_grabbed_devices.grab(virtual_device_name, false) ? 0 : 1;
#endif

  if (!g_client.initialize()) {
    error("Initializing keymapper connection failed");
    return 1;
  }

  return connection_loop();
}