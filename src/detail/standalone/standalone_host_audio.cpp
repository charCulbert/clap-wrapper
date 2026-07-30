

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

//#define MINIAUDIO_IMPLEMENTATION
//#include "miniaudio.h"
#include "RtAudio.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "standalone_host.h"
#include "entry.h"

namespace freeaudio::clap_wrapper::standalone
{
int rtaCallback(void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames,
                double /* streamTime */, RtAudioStreamStatus status, void *data)
{
  if (status)
  {
  }
  auto sh = (StandaloneHost *)data;
  sh->clapProcess(outputBuffer, inputBuffer, nBufferFrames);

  return 0;
}

void rtaErrorCallback(RtAudioErrorType errorType, const std::string &errorText)
{
  if (errorType != RTAUDIO_OUTPUT_UNDERFLOW && errorType != RTAUDIO_INPUT_OVERFLOW)
  {
    LOGINFO("[ERROR] RtAudio reports '{}' [{}]", errorText, (int)errorType);
    auto ae = getStandaloneHost()->displayAudioError;
    if (ae)
    {
      ae(errorText);
    }
  }
  else
  {
    static bool reported = false;
    if (!reported)
    {
      LOGINFO("[ERROR] RtAudio reports under/overflow '{}' [{}]", errorText, (int)errorType);
      reported = true;
    }
  }
}

void StandaloneHost::guaranteeRtAudioDAC()
{
  if (!rtaDac)
  {
    LOGDETAIL("Creating Standalone RtAudioDAC");
    setAudioApi(RtAudio::Api::UNSPECIFIED);
  }
}

void StandaloneHost::setAudioApi(RtAudio::Api api)
{
  rtaDac = std::make_unique<RtAudio>(api, &rtaErrorCallback);
  audioApi = api;
  audioApiName = rtaDac->getApiName(api);
  audioApiDisplayName = rtaDac->getApiDisplayName(api);
  rtaDac->showWarnings(true);
}

std::tuple<unsigned int, unsigned int, int32_t> StandaloneHost::getDefaultAudioInOutSampleRate()
{
  guaranteeRtAudioDAC();
  auto iid = rtaDac->getDefaultInputDevice();
  auto oid = rtaDac->getDefaultOutputDevice();
  auto outInfo = rtaDac->getDeviceInfo(oid);
  auto sr = outInfo.currentSampleRate;
  if (sr < 1)
  {
    sr = outInfo.preferredSampleRate;
  }

  return {iid, oid, (int32_t)sr};
}
bool StandaloneHost::startAudioThread()
{
  guaranteeRtAudioDAC();

  if (startupAudioSet)
  {
    auto in = startAudioIn;
    auto out = startAudioOut;
    auto sr = startSampleRate;
    return startAudioThreadOn(in, startAudioInputChannels, startAudioInputUsed && numAudioInputs > 0, out,
                              startAudioOutputChannels,
                              startAudioOutputUsed && numAudioOutputs > 0, sr);
  }

  auto [in, out, sr] = getDefaultAudioInOutSampleRate();
  return startAudioThreadOn(in, 2, numAudioInputs > 0, out, 2, numAudioOutputs > 0, sr);
}

std::vector<RtAudio::DeviceInfo> filterDevicesBy(const std::unique_ptr<RtAudio> &rtaDac,
                                                 std::function<bool(const RtAudio::DeviceInfo &)> f)
{
  std::vector<RtAudio::DeviceInfo> res;
  auto dids = rtaDac->getDeviceIds();
  auto dnms = rtaDac->getDeviceNames();
  for (auto d : dids)
  {
    auto inf = rtaDac->getDeviceInfo(d);
    if (f(inf))
    {
      res.push_back(inf);
    }
  }
  return res;
}

std::vector<RtAudio::Api> StandaloneHost::getCompiledApi()
{
  guaranteeRtAudioDAC();

  std::vector<RtAudio::Api> compiledApi;
  rtaDac->getCompiledApi(compiledApi);

  return compiledApi;
}

std::vector<RtAudio::DeviceInfo> StandaloneHost::getInputAudioDevices()
{
  guaranteeRtAudioDAC();
  return filterDevicesBy(rtaDac, [](auto &a) { return a.inputChannels > 0; });
}

std::vector<RtAudio::DeviceInfo> StandaloneHost::getOutputAudioDevices()
{
  guaranteeRtAudioDAC();
  return filterDevicesBy(rtaDac, [](auto &a) { return a.outputChannels > 0; });
}

std::vector<int32_t> StandaloneHost::getSampleRates()
{
  guaranteeRtAudioDAC();

  std::vector<int32_t> res;

  auto samplesRates{rtaDac->getDeviceInfo(audioInputDeviceID).sampleRates};

  for (auto &sampleRate : rtaDac->getDeviceInfo(audioInputDeviceID).sampleRates)
  {
    res.push_back(sampleRate);
  }

  return res;
}

std::vector<uint32_t> StandaloneHost::getBufferSizes()
{
  guaranteeRtAudioDAC();

  std::vector<uint32_t> res{16,  32,  48,  64,  96,  128,  144,  160,
                            192, 224, 256, 480, 512, 1024, 2048, 4096};
  return res;
}

bool StandaloneHost::startAudioThreadOn(unsigned int inputDeviceID, uint32_t inputChannels,
                                        bool useInput, unsigned int outputDeviceID,
                                        uint32_t outputChannels, bool useOutput,
                                        int32_t reqSampleRate)
{
  guaranteeRtAudioDAC();

  if (rtaDac->isStreamRunning())
    stopAudioThread();
  running.store(false, std::memory_order_release);

  audioInputDeviceID = inputDeviceID;
  audioInputUsed = useInput;
  audioOutputDeviceID = outputDeviceID;
  audioOutputUsed = useOutput;
  currentInputChannels = 0;
  currentOutputChannels = 0;

  auto dids = rtaDac->getDeviceIds();
  auto dnms = rtaDac->getDeviceNames();

  RtAudio::StreamParameters oParams;
  int32_t sampleRate{reqSampleRate};

  if (useOutput)
  {
    oParams.deviceId = outputDeviceID;
    auto outInfo = rtaDac->getDeviceInfo(oParams.deviceId);
    oParams.nChannels = std::min(outputChannels, outInfo.outputChannels);
    oParams.firstChannel = 0;
    if (sampleRate < 0)
    {
      sampleRate = outInfo.preferredSampleRate;
    }
    else
    {
      // Mkae sure this sample rate is available
      bool isPossible{false};
      for (auto sr : outInfo.sampleRates)
      {
        isPossible = isPossible || ((int)sr == (int)sampleRate);
      }
      if (!isPossible)
      {
        sampleRate = outInfo.preferredSampleRate;
      }
    }
  }

  RtAudio::StreamParameters iParams;
  if (useInput)
  {
    iParams.deviceId = inputDeviceID;
    auto inInfo = rtaDac->getDeviceInfo(iParams.deviceId);
    iParams.nChannels = std::min(inputChannels, inInfo.inputChannels);
    iParams.firstChannel = 0;
    if (sampleRate < 0) sampleRate = inInfo.preferredSampleRate;
  }

  if (sampleRate < 0)
  {
    LOGINFO("[WARNING] No preferred sample rate detected; using 48k");
    sampleRate = 48000;
  }

  currentSampleRate = sampleRate;

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_NONINTERLEAVED;

  /*
   * RTAudio doesn't tell you what the possible frame sizes are but instead
   * just tells you to try open stream with power of twos you want. So leave
   * this for now at 256 and return to it shortly.
   */
  LOGINFO("[WARNING] Hardcoding frame size to 256 samples for now");

  if (currentBufferSize == 0)
  {
    currentBufferSize = 256;
  }

  if (rtaDac->openStream((useOutput) ? &oParams : nullptr, (useInput) ? &iParams : nullptr,
                         RTAUDIO_FLOAT32, sampleRate, &currentBufferSize, &rtaCallback, (void *)this,
                         &options))
  {
    LOGINFO("[ERROR] Error opening rta stream '{}'", rtaDac->getErrorText());
    deactivatePlugin();
    rtaDac->closeStream();
    return false;
  }

  if (!activatePlugin(sampleRate, 1, currentBufferSize * 2))
  {
    LOGINFO("[ERROR] Plugin activation failed");
    running.store(false, std::memory_order_release);
    rtaDac->closeStream();
    return false;
  }

  LOGDETAIL("RtAudio Attached Devices");
  if (useOutput)
  {
    for (auto i = 0U; i < dids.size(); ++i)
    {
      if (oParams.deviceId == dids[i]) LOGDETAIL("  - Output : '{}'", dnms[i]);
    }
    currentOutputChannels = oParams.nChannels;
    LOGDETAIL("RtAudio Output Stream Channels {}", oParams.nChannels);
  }
  if (useInput)
  {
    for (auto i = 0U; i < dids.size(); ++i)
    {
      if (iParams.deviceId == dids[i]) LOGDETAIL("  - Input : '{}'", dnms[i]);
    }
    currentInputChannels = iParams.nChannels;
    LOGDETAIL("RtAudio Input Stream Channels {}", iParams.nChannels);
  }

  clap_wrapper_standalone_audio_settings_t currentSettings{};
  currentSettings.struct_size = sizeof(currentSettings);
  currentSettings.input_device_id = useInput ? inputDeviceID : 0;
  currentSettings.output_device_id = useOutput ? outputDeviceID : 0;
  currentSettings.input_channels = useInput ? currentInputChannels : 0;
  currentSettings.output_channels = useOutput ? currentOutputChannels : 0;
  currentSettings.sample_rate = static_cast<uint32_t>(sampleRate);
  currentSettings.buffer_size = currentBufferSize;
  if (useInput) currentSettings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED;
  if (useOutput) currentSettings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
  services.recordAudioSettings(currentSettings);

  if (!rtaDac->isStreamOpen())
  {
    LOGINFO("[ERROR] Stream failed to open :  {}", rtaDac->getErrorText());
    running.store(false, std::memory_order_release);
    deactivatePlugin();
    rtaDac->closeStream();
    return false;
  }

  if (rtaDac->startStream())
  {
    LOGINFO("[ERROR] startStream failed : {}", rtaDac->getErrorText());
    running.store(false, std::memory_order_release);
    deactivatePlugin();
    rtaDac->closeStream();
    return false;
  }

  running.store(true, std::memory_order_release);
  services.setAudioRunning(true);
  finishedRunning.store(false, std::memory_order_release);
  return true;
}

void StandaloneHost::stopAudioThread()
{
  LOGINFO("Shutting down audio");
  if (!rtaDac) return;

  services.setAudioRunning(false);

  if (!rtaDac->isStreamRunning())
  {
  }
  else
  {
    running = false;

    // bit of a hack. Wait until we get an ack from audio callback
    for (auto i = 0; i < 10000 && !finishedRunning; ++i)
    {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(1ms);
    }

    if (rtaDac && rtaDac->isStreamRunning())
    {
      rtaDac->stopStream();
      rtaDac->closeStream();
    }
  }
  return;
}
}  // namespace freeaudio::clap_wrapper::standalone
