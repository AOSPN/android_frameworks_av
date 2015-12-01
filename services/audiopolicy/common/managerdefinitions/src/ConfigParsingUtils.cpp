/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "APM::ConfigParsingUtils"
//#define LOG_NDEBUG 0

#include "ConfigParsingUtils.h"
#include <convert/convert.h>
#include "AudioGain.h"
#include "IOProfile.h"
#include "TypeConverter.h"
#include <hardware/audio.h>
#include <utils/Log.h>
#include <cutils/misc.h>

namespace android {

// --- audio_policy.conf file parsing
//static
uint32_t ConfigParsingUtils::parseOutputFlagNames(const char *name)
{
    uint32_t flag = OutputFlagConverter::maskFromString(name);
    //force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((flag & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        flag |= AUDIO_OUTPUT_FLAG_DIRECT;
    }
    return flag;
}

//static
void ConfigParsingUtils::loadAudioPortGain(cnode *root, AudioPort &audioPort, int index)
{
    cnode *node = root->first_child;

    sp<AudioGain> gain = new AudioGain(index, audioPort.useInputChannelMask());

    while (node) {
        if (strcmp(node->name, GAIN_MODE) == 0) {
            gain->setMode(GainModeConverter::maskFromString(node->value));
        } else if (strcmp(node->name, GAIN_CHANNELS) == 0) {
            audio_channel_mask_t mask;
            if (audioPort.useInputChannelMask()) {
                if (InputChannelConverter::fromString(node->value, mask)) {
                    gain->setChannelMask(mask);
                }
            } else {
                if (OutputChannelConverter::fromString(node->value, mask)) {
                    gain->setChannelMask(mask);
                }
            }
        } else if (strcmp(node->name, GAIN_MIN_VALUE) == 0) {
            gain->setMinValueInMb(atoi(node->value));
        } else if (strcmp(node->name, GAIN_MAX_VALUE) == 0) {
            gain->setMaxValueInMb(atoi(node->value));
        } else if (strcmp(node->name, GAIN_DEFAULT_VALUE) == 0) {
            gain->setDefaultValueInMb(atoi(node->value));
        } else if (strcmp(node->name, GAIN_STEP_VALUE) == 0) {
            gain->setStepValueInMb(atoi(node->value));
        } else if (strcmp(node->name, GAIN_MIN_RAMP_MS) == 0) {
            gain->setMinRampInMs(atoi(node->value));
        } else if (strcmp(node->name, GAIN_MAX_RAMP_MS) == 0) {
            gain->setMaxRampInMs(atoi(node->value));
        }
        node = node->next;
    }

    ALOGV("loadGain() adding new gain mode %08x channel mask %08x min mB %d max mB %d",
          gain->getMode(), gain->getChannelMask(), gain->getMinValueInMb(),
          gain->getMaxValueInMb());

    if (gain->getMode() == 0) {
        return;
    }
    audioPort.mGains.add(gain);
}

void ConfigParsingUtils::loadAudioPortGains(cnode *root, AudioPort &audioPort)
{
    cnode *node = root->first_child;
    int index = 0;
    while (node) {
        ALOGV("loadGains() loading gain %s", node->name);
        loadAudioPortGain(node, audioPort, index++);
        node = node->next;
    }
}

//static
void ConfigParsingUtils::loadDeviceDescriptorGains(cnode *root, sp<DeviceDescriptor> &deviceDesc)
{
    loadAudioPortGains(root, *deviceDesc);
    if (deviceDesc->mGains.size() > 0) {
        deviceDesc->mGains[0]->getDefaultConfig(&deviceDesc->mGain);
    }
}

//static
status_t ConfigParsingUtils::loadHwModuleDevice(cnode *root, DeviceVector &devices)
{
    cnode *node = root->first_child;

    audio_devices_t type = AUDIO_DEVICE_NONE;
    while (node) {
        if (strcmp(node->name, APM_DEVICE_TYPE) == 0) {
            DeviceConverter::fromString(node->value, type);
            break;
        }
        node = node->next;
    }
    if (type == AUDIO_DEVICE_NONE ||
            (!audio_is_input_device(type) && !audio_is_output_device(type))) {
        ALOGW("loadDevice() bad type %08x", type);
        return BAD_VALUE;
    }
    sp<DeviceDescriptor> deviceDesc = new DeviceDescriptor(type);
    deviceDesc->mTag = String8(root->name);

    node = root->first_child;
    while (node) {
        if (strcmp(node->name, APM_DEVICE_ADDRESS) == 0) {
            deviceDesc->mAddress = String8((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            if (audio_is_input_device(type)) {
                deviceDesc->setSupportedChannelMasks(inputChannelMasksFromString(node->value));
            } else {
                deviceDesc->setSupportedChannelMasks(outputChannelMasksFromString(node->value));
            }
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            loadDeviceDescriptorGains(node, deviceDesc);
        }
        node = node->next;
    }

    ALOGV("loadDevice() adding device tag %s type %08x address %s",
          deviceDesc->mTag.string(), type, deviceDesc->mAddress.string());

    devices.add(deviceDesc);
    return NO_ERROR;
}

//static
status_t ConfigParsingUtils::loadHwModuleInput(cnode *root, sp<HwModule> &module)
{
    cnode *node = root->first_child;

    sp<InputProfile> profile = new InputProfile(String8(root->name));

    while (node) {
        if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            profile->setSupportedSamplingRates(samplingRatesFromString(node->value));
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            profile->setSupportedFormats(formatsFromString(node->value));
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            profile->setSupportedChannelMasks(inputChannelMasksFromString(node->value));
        } else if (strcmp(node->name, DEVICES_TAG) == 0) {
            DeviceVector devices;
            loadDevicesFromTag(node->value, devices, module->getDeclaredDevices());
            profile->setSupportedDevices(devices);
        } else if (strcmp(node->name, FLAGS_TAG) == 0) {
            profile->setFlags(InputFlagConverter::maskFromString(node->value));
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            loadAudioPortGains(node, *profile);
        }
        node = node->next;
    }
    ALOGW_IF(profile->getSupportedDevices().isEmpty(),
            "loadInput() invalid supported devices");
    ALOGW_IF(profile->mChannelMasks.size() == 0,
            "loadInput() invalid supported channel masks");
    ALOGW_IF(profile->mSamplingRates.size() == 0,
            "loadInput() invalid supported sampling rates");
    ALOGW_IF(profile->mFormats.size() == 0,
            "loadInput() invalid supported formats");
    if (!profile->getSupportedDevices().isEmpty() &&
            (profile->mChannelMasks.size() != 0) &&
            (profile->mSamplingRates.size() != 0) &&
            (profile->mFormats.size() != 0)) {

        ALOGV("loadInput() adding input Supported Devices %04x",
              profile->getSupportedDevices().types());
        return module->addInputProfile(profile);
    } else {
        return BAD_VALUE;
    }
}

//static
status_t ConfigParsingUtils::loadHwModuleOutput(cnode *root, sp<HwModule> &module)
{
    cnode *node = root->first_child;

    sp<OutputProfile> profile = new OutputProfile(String8(root->name));

    while (node) {
        if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            profile->setSupportedSamplingRates(samplingRatesFromString(node->value));
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            profile->setSupportedFormats(formatsFromString(node->value));
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            profile->setSupportedChannelMasks(outputChannelMasksFromString(node->value));
        } else if (strcmp(node->name, DEVICES_TAG) == 0) {
            DeviceVector devices;
            loadDevicesFromTag(node->value, devices, module->getDeclaredDevices());
            profile->setSupportedDevices(devices);
        } else if (strcmp(node->name, FLAGS_TAG) == 0) {
            profile->setFlags(ConfigParsingUtils::parseOutputFlagNames(node->value));
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            loadAudioPortGains(node, *profile);
        }
        node = node->next;
    }
    ALOGW_IF(profile->getSupportedDevices().isEmpty(),
            "loadOutput() invalid supported devices");
    ALOGW_IF(profile->mChannelMasks.size() == 0,
            "loadOutput() invalid supported channel masks");
    ALOGW_IF(profile->mSamplingRates.size() == 0,
            "loadOutput() invalid supported sampling rates");
    ALOGW_IF(profile->mFormats.size() == 0,
            "loadOutput() invalid supported formats");
    if (!profile->getSupportedDevices().isEmpty() &&
            (profile->mChannelMasks.size() != 0) &&
            (profile->mSamplingRates.size() != 0) &&
            (profile->mFormats.size() != 0)) {

        ALOGV("loadOutput() adding output Supported Devices %04x, mFlags %04x",
              profile->getSupportedDevices().types(), profile->getFlags());
        return module->addOutputProfile(profile);
    } else {
        return BAD_VALUE;
    }
}

//static
status_t ConfigParsingUtils::loadHwModule(cnode *root, sp<HwModule> &module,
                                          AudioPolicyConfig &config)
{
    status_t status = NAME_NOT_FOUND;
    cnode *node = config_find(root, DEVICES_TAG);
    if (node != NULL) {
        node = node->first_child;
        DeviceVector devices;
        while (node) {
            ALOGV("loadHwModule() loading device %s", node->name);
            status_t tmpStatus = loadHwModuleDevice(node, devices);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
        module->setDeclaredDevices(devices);
    }
    node = config_find(root, OUTPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading output %s", node->name);
            status_t tmpStatus = loadHwModuleOutput(node, module);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    node = config_find(root, INPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading input %s", node->name);
            status_t tmpStatus = loadHwModuleInput(node, module);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    loadModuleGlobalConfig(root, module, config);
    return status;
}

//static
void ConfigParsingUtils::loadHwModules(cnode *root, HwModuleCollection &hwModules,
                                       AudioPolicyConfig &config)
{
    cnode *node = config_find(root, AUDIO_HW_MODULE_TAG);
    if (node == NULL) {
        return;
    }

    node = node->first_child;
    while (node) {
        ALOGV("loadHwModules() loading module %s", node->name);
        sp<HwModule> module = new HwModule(node->name);
        if (loadHwModule(node, module, config) == NO_ERROR) {
            hwModules.add(module);
        }
        node = node->next;
    }
}

//static
void ConfigParsingUtils::loadDevicesFromTag(const char *tag, DeviceVector &devices,
                                            const DeviceVector &declaredDevices)
{
    char *tagLiteral = strndup(tag, strlen(tag));
    char *devTag = strtok(tagLiteral, "|");
    while (devTag != NULL) {
        if (strlen(devTag) != 0) {
            audio_devices_t type;
            if (DeviceConverter::fromString(devTag, type)) {
                sp<DeviceDescriptor> dev = new DeviceDescriptor(type);
                if (type == AUDIO_DEVICE_IN_REMOTE_SUBMIX ||
                        type == AUDIO_DEVICE_OUT_REMOTE_SUBMIX ) {
                    dev->mAddress = String8("0");
                }
                devices.add(dev);
            } else {
                sp<DeviceDescriptor> deviceDesc =
                        declaredDevices.getDeviceFromTag(String8(devTag));
                if (deviceDesc != 0) {
                    devices.add(deviceDesc);
                }
            }
        }
        devTag = strtok(NULL, "|");
    }
    free(tagLiteral);
}

//static
void ConfigParsingUtils::loadModuleGlobalConfig(cnode *root, const sp<HwModule> &module,
                                                AudioPolicyConfig &config)
{
    cnode *node = config_find(root, GLOBAL_CONFIG_TAG);

    if (node == NULL) {
        return;
    }
    DeviceVector declaredDevices;
    if (module != NULL) {
        declaredDevices = module->getDeclaredDevices();
    }

    node = node->first_child;
    while (node) {
        if (strcmp(ATTACHED_OUTPUT_DEVICES_TAG, node->name) == 0) {
            DeviceVector availableOutputDevices;
            loadDevicesFromTag(node->value, availableOutputDevices, declaredDevices);
            ALOGV("loadGlobalConfig() Attached Output Devices %08x",
                  availableOutputDevices.types());
            config.addAvailableOutputDevices(availableOutputDevices);
        } else if (strcmp(DEFAULT_OUTPUT_DEVICE_TAG, node->name) == 0) {
            audio_devices_t device = AUDIO_DEVICE_NONE;
            DeviceConverter::fromString(node->value, device);
            if (device != AUDIO_DEVICE_NONE) {
                sp<DeviceDescriptor> defaultOutputDevice = new DeviceDescriptor(device);
                config.setDefaultOutputDevice(defaultOutputDevice);
                ALOGV("loadGlobalConfig() mDefaultOutputDevice %08x", defaultOutputDevice->type());
            } else {
                ALOGW("loadGlobalConfig() default device not specified");
            }
        } else if (strcmp(ATTACHED_INPUT_DEVICES_TAG, node->name) == 0) {
            DeviceVector availableInputDevices;
            loadDevicesFromTag(node->value, availableInputDevices, declaredDevices);
            ALOGV("loadGlobalConfig() Available InputDevices %08x", availableInputDevices.types());
            config.addAvailableInputDevices(availableInputDevices);
        } else if (strcmp(AUDIO_HAL_VERSION_TAG, node->name) == 0) {
            uint32_t major, minor;
            sscanf((char *)node->value, "%u.%u", &major, &minor);
            module->setHalVersion(HARDWARE_DEVICE_API_VERSION(major, minor));
            ALOGV("loadGlobalConfig() mHalVersion = %04x major %u minor %u",
                  module->getHalVersion(), major, minor);
        }
        node = node->next;
    }
}

//static
void ConfigParsingUtils::loadGlobalConfig(cnode *root, AudioPolicyConfig &config,
                                          const sp<HwModule>& primaryModule)
{
    cnode *node = config_find(root, GLOBAL_CONFIG_TAG);

    if (node == NULL) {
        return;
    }
    node = node->first_child;
    while (node) {
        if (strcmp(SPEAKER_DRC_ENABLED_TAG, node->name) == 0) {
            bool speakerDrcEnabled;
            if (utilities::convertTo<std::string, bool>(node->value, speakerDrcEnabled)) {
                ALOGV("loadGlobalConfig() mSpeakerDrcEnabled = %d", speakerDrcEnabled);
                config.setSpeakerDrcEnabled(speakerDrcEnabled);
            }
        }
        node = node->next;
    }
    loadModuleGlobalConfig(root, primaryModule, config);
}

//static
status_t ConfigParsingUtils::loadConfig(const char *path, AudioPolicyConfig &config)
{
    cnode *root;
    char *data;

    data = (char *)load_file(path, NULL);
    if (data == NULL) {
        return -ENODEV;
    }
    root = config_node("", "");
    config_load(root, data);

    HwModuleCollection hwModules;
    loadHwModules(root, hwModules, config);

    // legacy audio_policy.conf files have one global_configuration section, attached to primary.
    loadGlobalConfig(root, config, hwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_PRIMARY));

    config.setHwModules(hwModules);

    config_free(root);
    free(root);
    free(data);

    ALOGI("loadAudioPolicyConfig() loaded %s\n", path);

    return NO_ERROR;
}

}; // namespace android
