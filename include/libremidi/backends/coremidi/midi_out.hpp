#pragma once
#include <libremidi/backends/coremidi/config.hpp>
#include <libremidi/backends/coremidi/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

namespace libremidi
{
class midi_out_core final
    : public midi1::out_api
    , private coremidi_data
    , public error_handler
{
public:
  struct
      : output_configuration
      , coremidi_output_configuration
  {
  } configuration;

  midi_out_core(output_configuration&& conf, coremidi_output_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
    if (auto result = init_client(configuration); result != noErr)
    {
      error(
          this->configuration,
          "midi_out_core: error creating MIDI client object: " + std::to_string(result));
      return;
    }
  }

  ~midi_out_core()
  {
    midi_out_core::close_port();

    if (this->endpoint)
      MIDIEndpointDispose(this->endpoint);

    close_client();
  }

  void close_client()
  {
    if (!configuration.context)
      MIDIClientDispose(this->client);
  }

  libremidi::API get_current_api() const noexcept override { return libremidi::API::COREMIDI; }

  stdx::error open_port(const output_port& info, std::string_view portName) override
  {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);

    // Find where we want to send
    auto destination = locate_object(*this, info, kMIDIObjectType_Destination);
    if (destination == 0)
      return std::make_error_code(std::errc::invalid_argument);

    // Create our local source
    MIDIPortRef port;
    OSStatus result = MIDIOutputPortCreate(this->client, toCFString(portName).get(), &port);
    if (result != noErr)
    {
      close_client();
      error(
          this->configuration, "midi_out_core::open_port: error creating macOS MIDI output port.");
      return from_osstatus(result);
    }

    // Save our api-specific connection information.
    this->port = port;
    this->destinationId = destination;

    return stdx::error{};
  }

  stdx::error open_virtual_port(std::string_view portName) override
  {
    // Create a virtual MIDI output source.
    MIDIEndpointRef endpoint;
    OSStatus result = MIDISourceCreate(this->client, toCFString(portName).get(), &endpoint);

    if (result != noErr)
    {
      error(
          this->configuration,
          "midi_out_core::initialize: error creating macOS virtual MIDI source.");

      return from_osstatus(result);
    }

    // Save our api-specific connection information.
    this->endpoint = endpoint;
    return stdx::error{};
  }

  stdx::error close_port() override
  {
    return coremidi_data::close_port();
  }

  stdx::error send_message(const unsigned char* message, size_t size) override
  {
    unsigned int nBytes = static_cast<unsigned int>(size);
    if (nBytes == 0)
    {
      warning(configuration, "midi_out_core::send_message: no data in message argument!");
      return std::make_error_code(std::errc::invalid_argument);
    }

    if (message[0] != 0xF0 && nBytes > 3)
    {
      warning(
          configuration,
          "midi_out_core::send_message: message format problem ... not sysex but "
          "> 3 bytes?");
      return std::make_error_code(std::errc::bad_message);
    }

    const MIDITimeStamp timestamp = LIBREMIDI_AUDIO_GET_CURRENT_HOST_TIME();

    const ByteCount bufsize = nBytes > 65535 ? 65535 : nBytes;
    Byte buffer[bufsize + 16]; // pad for other struct members
    ByteCount listSize = sizeof(buffer);
    MIDIPacketList* packetList = (MIDIPacketList*)buffer;

    ByteCount remainingBytes = nBytes;
    while (remainingBytes)
    {
      MIDIPacket* packet = MIDIPacketListInit(packetList);
      // A MIDIPacketList can only contain a maximum of 64K of data, so if our message is longer,
      // break it up into chunks of 64K or less and send out as a MIDIPacketList with only one
      // MIDIPacket. Here, we reuse the memory allocated above on the stack for all.
      ByteCount bytesForPacket = remainingBytes > 65535 ? 65535 : remainingBytes;
      const Byte* dataStartPtr = (const Byte*)&message[nBytes - remainingBytes];
      packet = MIDIPacketListAdd(
          packetList, listSize, packet, timestamp, bytesForPacket, dataStartPtr);
      remainingBytes -= bytesForPacket;

      if (!packet)
      {
        error(
            this->configuration, "midi_out_core::send_message: could not allocate packet list");

        return std::make_error_code(std::errc::message_size);
      }

      // Send to any destinations that may have connected to us.
      if (this->endpoint)
      {
        auto result = MIDIReceived(this->endpoint, packetList);
        if (result != noErr)
        {
          warning(
              this->configuration,
              "midi_out_core::send_message: error sending MIDI to virtual "
              "destinations.");
          return std::make_error_code(std::errc::io_error);
        }
      }

      // And send to an explicit destination port if we're connected.
      if (this->destinationId != 0)
      {
        auto result = MIDISend(this->port, this->destinationId, packetList);
        if (result != noErr)
        {
          warning(
              this->configuration,
              "midi_out_core::send_message: error sending MIDI message to port.");
          return std::make_error_code(std::errc::io_error);
        }
      }
    }
    return stdx::error{};
  }

  MIDIEndpointRef destinationId{};
};
}
