// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// clang-format off
#include "openvino/openvino.hpp"
#include "openvino/pass/serialize.hpp"

#include "gna/gna_config.hpp"
#include "gpu/gpu_config.hpp"

#include "samples/args_helper.hpp"
#include "samples/common.hpp"
#include "samples/slog.hpp"

#include "benchmark_app.hpp"
#include "infer_request_wrap.hpp"
#include "inputs_filling.hpp"
#include "remote_tensors_filling.hpp"
#include "statistics_report.hpp"
#include "utils.hpp"
// clang-format on

namespace {
bool parse_and_check_command_line(int argc, char* argv[]) {
    // ---------------------------Parsing and validating input
    // arguments--------------------------------------
    slog::info << "Parsing input parameters" << slog::endl;
    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_help || FLAGS_h) {
        show_usage();
        showAvailableDevices();
        return false;
    }

    if (FLAGS_m.empty()) {
        show_usage();
        throw std::logic_error("Model is required but not set. Please set -m option.");
    }

    if (FLAGS_latency_percentile > 100 || FLAGS_latency_percentile < 1) {
        show_usage();
        throw std::logic_error("The percentile value is incorrect. The applicable values range is [1, 100].");
    }
    if (FLAGS_api != "async" && FLAGS_api != "sync") {
        throw std::logic_error("Incorrect API. Please set -api option to `sync` or `async` value.");
    }
    if (!FLAGS_hint.empty() && FLAGS_hint != "throughput" && FLAGS_hint != "tput" && FLAGS_hint != "latency" &&
        FLAGS_hint != "cumulative_throughput" && FLAGS_hint != "ctput" && FLAGS_hint != "none") {
        throw std::logic_error("Incorrect performance hint. Please set -hint option to"
                               "`throughput`(tput), `latency', 'cumulative_throughput'(ctput) value or 'none'.");
    }
    if (FLAGS_hint != "none" && (FLAGS_nstreams != "" || FLAGS_nthreads != 0 || FLAGS_pin != "")) {
        throw std::logic_error("-nstreams, -nthreads and -pin options are fine tune options. To use them you "
                               "should explicitely set -hint option to none. This is not OpenVINO limitation "
                               "(those options can be used in OpenVINO together), but a benchmark_app UI rule.");
    }
    if (!FLAGS_report_type.empty() && FLAGS_report_type != noCntReport && FLAGS_report_type != averageCntReport &&
        FLAGS_report_type != detailedCntReport && FLAGS_report_type != sortDetailedCntReport) {
        std::string err = "only " + std::string(noCntReport) + "/" + std::string(averageCntReport) + "/" +
                          std::string(detailedCntReport) + "/" + std::string(sortDetailedCntReport) +
                          " report types are supported (invalid -report_type option value)";
        throw std::logic_error(err);
    }

    if ((FLAGS_report_type == averageCntReport) && ((FLAGS_d.find("MULTI") != std::string::npos))) {
        throw std::logic_error("only " + std::string(detailedCntReport) + " report type is supported for MULTI device");
    }

    if (!FLAGS_pcsort.empty() && FLAGS_pcsort != "sort" && FLAGS_pcsort != "no_sort" && FLAGS_pcsort != "simple_sort") {
        std::string pcsort_err = std::string("Incorrect performance count sort . Please set -pcsort option to ") +
                                 std::string("'sort', 'no_sort', 'simple_sort'.");
        throw std::logic_error(pcsort_err);
    }

    bool isNetworkCompiled = fileExt(FLAGS_m) == "blob";
    bool isPrecisionSet = !(FLAGS_ip.empty() && FLAGS_op.empty() && FLAGS_iop.empty());
    if (isNetworkCompiled && isPrecisionSet) {
        std::string err = std::string("Cannot set precision for a compiled model. ") +
                          std::string("Please re-compile your model with required precision "
                                      "using compile_tool");

        throw std::logic_error(err);
    }
    return true;
}

void next_step(const std::string additional_info = "") {
    static size_t step_id = 0;
    static const std::map<size_t, std::string> step_names = {{1, "Parsing and validating input arguments"},
                                                             {2, "Loading OpenVINO Runtime"},
                                                             {3, "Setting device configuration"},
                                                             {4, "Reading model files"},
                                                             {5, "Resizing model to match image sizes and given batch"},
                                                             {6, "Configuring input of the model"},
                                                             {7, "Loading the model to the device"},
                                                             {8, "Querying optimal runtime parameters"},
                                                             {9, "Creating infer requests and preparing input tensors"},
                                                             {10, "Measuring performance"},
                                                             {11, "Dumping statistics report"}};

    step_id++;

    OPENVINO_ASSERT(step_names.count(step_id) != 0,
                    "Step ID ",
                    step_id,
                    " is out of total steps number ",
                    step_names.size());

    std::cout << "[Step " << step_id << "/" << step_names.size() << "] " << step_names.at(step_id)
              << (additional_info.empty() ? "" : " (" + additional_info + ")") << std::endl;
}

ov::hint::PerformanceMode get_performance_hint(const std::string& device, const ov::Core& core) {
    ov::hint::PerformanceMode ov_perf_hint = ov::hint::PerformanceMode::UNDEFINED;
    auto supported_properties = core.get_property(device, ov::supported_properties);
    if (std::find(supported_properties.begin(), supported_properties.end(), ov::hint::performance_mode) !=
        supported_properties.end()) {
        if (FLAGS_hint != "") {
            if (FLAGS_hint == "throughput" || FLAGS_hint == "tput") {
                slog::warn << "Device(" << device << ") performance hint is set to THROUGHPUT" << slog::endl;
                ov_perf_hint = ov::hint::PerformanceMode::THROUGHPUT;
            } else if (FLAGS_hint == "latency") {
                slog::warn << "Device(" << device << ") performance hint is set to LATENCY" << slog::endl;
                ov_perf_hint = ov::hint::PerformanceMode::LATENCY;
            } else if (FLAGS_hint == "cumulative_throughput" || FLAGS_hint == "ctput") {
                slog::warn << "Device(" << device << ") performance hint is set to CUMULATIVE_THROUGHPUT" << slog::endl;
                ov_perf_hint = ov::hint::PerformanceMode::CUMULATIVE_THROUGHPUT;
            } else if (FLAGS_hint == "none") {
                slog::warn << "No device(" << device << ") performance hint is set" << slog::endl;
                ov_perf_hint = ov::hint::PerformanceMode::UNDEFINED;
            }
        } else {
            ov_perf_hint =
                FLAGS_api == "async" ? ov::hint::PerformanceMode::THROUGHPUT : ov::hint::PerformanceMode::LATENCY;

            slog::warn << "Performance hint was not explicitly specified in command line. "
                          "Device("
                       << device << ") performance hint will be set to " << ov_perf_hint << "." << slog::endl;
        }
    } else {
        if (FLAGS_hint != "") {
            slog::warn << "Device(" << device << ") does not support performance hint property(-hint)." << slog::endl;
        }
    }
    return ov_perf_hint;
}

void setDeviceProperty(ov::Core& core,
                       std::string& device,
                       ov::AnyMap& device_config,
                       const std::pair<std::string, ov::Any>& property,
                       std::map<std::string, bool>& is_dev_set_property,
                       const std::pair<std::string, ov::Any>& config = {}) {
    auto supported_properties = core.get_property(device, ov::supported_properties);
    auto supported = [&](const std::string& key) {
        return std::find(std::begin(supported_properties), std::end(supported_properties), key) !=
               std::end(supported_properties);
    };
    // check if the HW device supported this property
    std::pair<std::string, ov::Any> device_property;
    if (!config.first.empty() && supported(config.first)) {
        device_property = config;
    } else if (supported(property.first))
        device_property = property;

    if (device_property.first.empty())
        return;

    if (device_config.find(device) == device_config.end() ||  // device properties not existed
        config.first.empty() &&                               // not setting default value to property
            (!FLAGS_load_config.empty() &&
             is_dev_set_property[device])) {  // device properties loaded from file and overwrite is not happened
        is_dev_set_property[device] = false;
        device_config.erase(device);
        device_config.insert(ov::device::properties(device, device_property));
    } else {
        auto& properties = device_config[device].as<ov::AnyMap>();
        // property present in device properties has higher priority than property set with default value
        properties.emplace(device_property);
    }
}

void warn_if_no_batch(const benchmark_app::InputsInfo& first_inputs) {
    if (!std::any_of(first_inputs.begin(),
                     first_inputs.end(),
                     [](const std::pair<const std::string, benchmark_app::InputInfo>& info) {
                         return ov::layout::has_batch(info.second.layout);
                     })) {
        slog::warn
            << "No batch dimension was found, asssuming batch to be 1. Beware: this might affect FPS calculation."
            << slog::endl;
    }
}

void fuse_mean_scale(ov::preprocess::PrePostProcessor& preproc, const benchmark_app::InputsInfo& app_inputs_info) {
    // TODO: remove warning after 23.3 release
    bool warned = false;
    constexpr char warn_msg[] = "Mean/scale values are fused into the model. This slows down performance compared to "
                                "--imean and --iscale which existed before";
    for (const std::pair<std::string, benchmark_app::InputInfo>& input_info : app_inputs_info) {
        if (!input_info.second.mean.empty()) {
            if (!warned) {
                slog::warn << warn_msg << slog::endl;
                warned = true;
            }
            preproc.input(input_info.first)
                .preprocess()
                .convert_element_type(ov::element::f32)
                .mean(input_info.second.mean);
        }
        if (!input_info.second.scale.empty()) {
            if (!warned) {
                slog::warn << warn_msg << slog::endl;
                warned = true;
            }
            preproc.input(input_info.first)
                .preprocess()
                .convert_element_type(ov::element::f32)
                .scale(input_info.second.scale);
        }
    }
}
}  // namespace

/**
 * @brief The entry point of the benchmark application
 */
int main(int argc, char* argv[]) {
    std::shared_ptr<StatisticsReport> statistics;
    try {
        ov::CompiledModel compiledModel;

        // ----------------- 1. Parsing and validating input arguments
        // -------------------------------------------------
        next_step();

        if (!parse_and_check_command_line(argc, argv)) {
            return 0;
        }

        bool isNetworkCompiled = fileExt(FLAGS_m) == "blob";
        if (isNetworkCompiled) {
            slog::info << "Model is compiled" << slog::endl;
        }

        std::vector<gflags::CommandLineFlagInfo> flags;
        StatisticsReport::Parameters command_line_arguments;
        gflags::GetAllFlags(&flags);
        for (auto& flag : flags) {
            if (!flag.is_default) {
                command_line_arguments.emplace_back(flag.name, flag.name, flag.current_value);
            }
        }
        if (!FLAGS_report_type.empty()) {
            statistics = FLAGS_json_stats ? std::make_shared<StatisticsReportJSON>(
                                                StatisticsReport::Config{FLAGS_report_type, FLAGS_report_folder})
                                          : std::make_shared<StatisticsReport>(
                                                StatisticsReport::Config{FLAGS_report_type, FLAGS_report_folder});

            statistics->add_parameters(StatisticsReport::Category::COMMAND_LINE_PARAMETERS, command_line_arguments);
        }
        auto isFlagSetInCommandLine = [&command_line_arguments](const std::string& name) {
            return (std::find_if(command_line_arguments.begin(),
                                 command_line_arguments.end(),
                                 [name](const StatisticsVariant& p) {
                                     return p.json_name == name;
                                 }) != command_line_arguments.end());
        };

        std::string device_name = FLAGS_d;

        // Parse devices
        auto devices = parse_devices(device_name);

        std::map<std::string, bool> is_dev_set_property = {};
        // initialize flags to ensure ov::device::properties should only be set once per device.
        for (auto& dev : devices)
            is_dev_set_property[dev] = true;
        // Parse nstreams per device
        std::map<std::string, std::string> device_nstreams = parse_value_per_device(devices, FLAGS_nstreams);
        std::map<std::string, std::string> device_infer_precision =
            parse_value_per_device(devices, FLAGS_infer_precision);

        // Load device config file if specified
        std::map<std::string, ov::AnyMap> config;
        bool is_load_config = false;
        if (!FLAGS_load_config.empty()) {
            load_config(FLAGS_load_config, config);
            is_load_config = true;
        }

        /** This vector stores paths to the processed images with input names**/
        auto inputFiles = parse_input_arguments(gflags::GetArgvs());

        // ----------------- 2. Loading the OpenVINO Runtime
        // -----------------------------------------------------------
        next_step();

        ov::Core core;

        if (!FLAGS_extensions.empty()) {
            // Extensions are loaded as a shared library
            core.add_extension(FLAGS_extensions);
            slog::info << "Extensions are loaded: " << FLAGS_extensions << slog::endl;
        }

        // Load clDNN Extensions
        if ((FLAGS_d.find("GPU") != std::string::npos) && !FLAGS_c.empty()) {
            // Override config if command line parameter is specified
            if (!config.count("GPU"))
                config["GPU"] = {};
            config["GPU"][CONFIG_KEY(CONFIG_FILE)] = FLAGS_c;
        }
        if (config.count("GPU") && config.at("GPU").count(CONFIG_KEY(CONFIG_FILE))) {
            auto ext = config.at("GPU").at(CONFIG_KEY(CONFIG_FILE)).as<std::string>();
            core.set_property("GPU", {{CONFIG_KEY(CONFIG_FILE), ext}});
            slog::info << "GPU extensions are loaded: " << ext << slog::endl;
        }

        slog::info << "OpenVINO:" << slog::endl;
        slog::info << ov::get_openvino_version() << slog::endl;
        slog::info << "Device info:" << slog::endl;
        slog::info << core.get_versions(device_name) << slog::endl;

        // ----------------- 3. Setting device configuration
        // -----------------------------------------------------------
        next_step();

        auto getDeviceTypeFromName = [](std::string device) -> std::string {
            return device.substr(0, device.find_first_of(".("));
        };

        // Set default values from dumped config
        std::set<std::string> default_devices;
        for (auto& device : devices) {
            auto default_config = config.find(getDeviceTypeFromName(device));
            if (default_config != config.end()) {
                if (!config.count(device)) {
                    config[device] = default_config->second;
                    default_devices.emplace(default_config->first);
                }
            }
        }
        for (auto& device : default_devices) {
            config.erase(device);
        }

        bool perf_counts = false;
        // check if using the virtual device
        auto if_auto = std::find(devices.begin(), devices.end(), "AUTO") != devices.end();
        auto if_multi = std::find(devices.begin(), devices.end(), "MULTI") != devices.end();
        auto hardware_devices = devices;
        // Remove the hardware devices if AUTO/MULTI appears in the devices list.
        if (if_auto || if_multi) {
            devices.clear();
            // Parse out the currect virtual device as the target device.
            std::string virtual_device = split(device_name, ':').at(0);
            auto iter_virtual = std::find(hardware_devices.begin(), hardware_devices.end(), virtual_device);
            hardware_devices.erase(iter_virtual);
            devices.push_back(virtual_device);
            parse_value_for_virtual_device(virtual_device, device_nstreams);
            parse_value_for_virtual_device(virtual_device, device_infer_precision);
        }

        // Update config per device according to command line parameters
        for (auto& device : devices) {
            auto& device_config = config[device];

            // high-level performance modes
            auto ov_perf_hint = get_performance_hint(device, core);
            device_config.emplace(ov::hint::performance_mode(ov_perf_hint));
            if (FLAGS_nireq != 0)
                device_config.emplace(ov::hint::num_requests(unsigned(FLAGS_nireq)));

            // Set performance counter
            if (isFlagSetInCommandLine("pc")) {
                // set to user defined value
                device_config.emplace(ov::enable_profiling(FLAGS_pc));
            } else if (device_config.count(ov::enable_profiling.name()) &&
                       (device_config.at(ov::enable_profiling.name()).as<bool>())) {
                slog::warn << "Performance counters for " << device
                           << " device is turned on. To print results use -pc option." << slog::endl;
            } else if (FLAGS_report_type == detailedCntReport || FLAGS_report_type == averageCntReport ||
                       FLAGS_report_type == sortDetailedCntReport) {
                slog::warn << "Turn on performance counters for " << device << " device since report type is "
                           << FLAGS_report_type << "." << slog::endl;
                device_config.emplace(ov::enable_profiling(true));
            } else if (!FLAGS_exec_graph_path.empty()) {
                slog::warn << "Turn on performance counters for " << device << " device due to execution graph dumping."
                           << slog::endl;
                device_config.emplace(ov::enable_profiling(true));
            } else if (!FLAGS_pcsort.empty()) {
                slog::warn << "Turn on sorted performance counters for " << device << " device since pcsort value is "
                           << FLAGS_pcsort << "." << slog::endl;
                device_config.emplace(ov::enable_profiling(true));
            } else {
                // set to default value
                device_config.emplace(ov::enable_profiling(FLAGS_pc));
            }
            perf_counts = (device_config.at(ov::enable_profiling.name()).as<bool>()) ? true : perf_counts;

            auto supported_properties = core.get_property(device, ov::supported_properties);

            auto supported = [&](const std::string& key) {
                return std::find(std::begin(supported_properties), std::end(supported_properties), key) !=
                       std::end(supported_properties);
            };
            // the rest are individual per-device settings (overriding the values set with perf modes)
            auto setThroughputStreams = [&]() {
                std::string key = getDeviceTypeFromName(device) + "_THROUGHPUT_STREAMS";
                auto it_device_nstreams = device_nstreams.find(device);
                if (it_device_nstreams != device_nstreams.end()) {
                    // set to user defined value
                    if (supported(key)) {
                        device_config[key] = it_device_nstreams->second;
                    } else if (supported(ov::num_streams.name())) {
                        // Use API 2.0 key for streams
                        key = ov::num_streams.name();
                        device_config[key] = it_device_nstreams->second;
                    } else if (device == "MULTI" || device == "AUTO") {
                        // check if the element contains the hardware device property
                        auto value_vec = split(it_device_nstreams->second, ' ');
                        if (value_vec.size() == 1) {
                            key = ov::num_streams.name();
                            device_config[key] = it_device_nstreams->second;
                        } else {
                            // set device nstreams properties in the AUTO/MULTI plugin
                            std::stringstream strm(it_device_nstreams->second);
                            std::map<std::string, std::string> devices_property;
                            ov::util::Read<std::map<std::string, std::string>>{}(strm, devices_property);
                            for (auto it : devices_property) {
                                if (device_config.find(it.first) == device_config.end() ||
                                    (is_load_config && is_dev_set_property[it.first])) {
                                    // Create ov::device::properties with ov::num_stream and
                                    // 1. Insert this ov::device::properties into device config if this
                                    // ov::device::properties isn't existed. Otherwise,
                                    // 2. Replace the existed ov::device::properties within device config.
                                    is_dev_set_property[it.first] = false;
                                    device_config.erase(it.first);
                                    device_config.insert(
                                        ov::device::properties(it.first, ov::num_streams(std::stoi(it.second))));
                                } else {
                                    auto& property = device_config[it.first].as<ov::AnyMap>();
                                    property.emplace(ov::num_streams(std::stoi(it.second)));
                                }
                            }
                        }
                    } else {
                        throw std::logic_error("Device " + device + " doesn't support config key '" + key + "' " +
                                               "and '" + ov::num_streams.name() + "'!" +
                                               "Please specify -nstreams for correct devices in format  "
                                               "<dev1>:<nstreams1>,<dev2>:<nstreams2>" +
                                               " or via configuration file.");
                    }
                } else if (ov_perf_hint == ov::hint::PerformanceMode::UNDEFINED && !device_config.count(key) &&
                           (FLAGS_api == "async")) {
                    slog::warn << "-nstreams default value is determined automatically for " << device
                               << " device. "
                                  "Although the automatic selection usually provides a "
                                  "reasonable performance, "
                                  "but it still may be non-optimal for some cases, for more "
                                  "information look at README."
                               << slog::endl;
                    if (device.find("MYRIAD") == std::string::npos) {  // MYRIAD sets the default number of
                                                                       // streams implicitly (without _AUTO)
                        if (supported(key)) {
                            device_config[key] = std::string(getDeviceTypeFromName(device) + "_THROUGHPUT_AUTO");
                        } else if (supported(ov::num_streams.name())) {
                            // Use API 2.0 key for streams
                            key = ov::num_streams.name();
                            device_config[key] = ov::streams::AUTO;
                        } else if (device == "MULTI" || device == "AUTO") {
                            // Set nstreams to default value auto if no nstreams specified from cmd line.
                            for (auto& hwdevice : hardware_devices) {
                                std::string key = std::string(getDeviceTypeFromName(hwdevice) + "_THROUGHPUT_STREAMS");
                                auto value = std::string(getDeviceTypeFromName(hwdevice) + "_THROUGHPUT_AUTO");
                                setDeviceProperty(core,
                                                  hwdevice,
                                                  device_config,
                                                  ov::num_streams(ov::streams::AUTO),
                                                  is_dev_set_property,
                                                  std::make_pair(key, value));
                            }
                        }
                    }
                }
                auto it_streams = device_config.find(ov::num_streams.name());
                if (it_streams != device_config.end())
                    device_nstreams[device] = it_streams->second.as<std::string>();
            };

            auto set_infer_precision = [&] {
                auto it_device_infer_precision = device_infer_precision.find(device);
                if (it_device_infer_precision != device_infer_precision.end()) {
                    // set to user defined value
                    if (supported(ov::hint::inference_precision.name())) {
                        device_config.emplace(ov::hint::inference_precision(it_device_infer_precision->second));
                    } else if (device == "MULTI" || device == "AUTO") {
                        // check if the element contains the hardware device property
                        auto value_vec = split(it_device_infer_precision->second, ' ');
                        if (value_vec.size() == 1) {
                            auto key = ov::hint::inference_precision.name();
                            device_config[key] = it_device_infer_precision->second;
                        } else {
                            // set device inference_precison properties in the AUTO/MULTI plugin
                            std::stringstream strm(it_device_infer_precision->second);
                            std::map<std::string, std::string> devices_property;
                            ov::util::Read<std::map<std::string, std::string>>{}(strm, devices_property);
                            for (auto it : devices_property) {
                                if (device_config.find(it.first) == device_config.end() ||
                                    (is_load_config && is_dev_set_property[it.first])) {
                                    // Create ov::device::properties with ov::inference_precision and
                                    // 1. Insert this ov::device::properties into device config if this
                                    // ov::device::properties isn't existed. Otherwise,
                                    // 2. Replace the existed ov::device::properties within device config.
                                    is_dev_set_property[it.first] = false;
                                    device_config.erase(it.first);
                                    device_config.insert(
                                        ov::device::properties(it.first, ov::hint::inference_precision(it.second)));
                                } else {
                                    auto& property = device_config[it.first].as<ov::AnyMap>();
                                    property.emplace(ov::hint::inference_precision(it.second));
                                }
                            }
                        }
                    } else {
                        throw std::logic_error("Device " + device + " doesn't support config key '" +
                                               ov::hint::inference_precision.name() + "'! " +
                                               "Please specify -infer_precision for correct devices in format  "
                                               "<dev1>:<infer_precision1>,<dev2>:<infer_precision2>" +
                                               " or via configuration file.");
                    }
                }
            };

            auto fix_pin_option = [](const std::string& str) -> std::string {
                if (str == "NO")
                    return "NONE";
                else if (str == "YES")
                    return "CORE";
                else
                    return str;
            };

            auto set_nthreads_pin = [&](const std::string& str) {
                auto property_name = str == "nthreads" ? ov::inference_num_threads.name() : ov::affinity.name();
                auto property = str == "nthreads" ? ov::inference_num_threads(int(FLAGS_nthreads))
                                                  : ov::affinity(fix_pin_option(FLAGS_pin));
                if (supported(property_name) || device_name == "AUTO") {
                    // create nthreads/pin primary property for HW device or AUTO if -d is AUTO directly.
                    device_config.emplace(property);
                } else if (if_auto || if_multi) {
                    // Create secondary property of -nthreads/-pin only for CPU if CPU device appears in the devices
                    // list specified by -d.
                    for (auto& device : hardware_devices) {
                        if (device == "CPU")
                            setDeviceProperty(core, device, device_config, property, is_dev_set_property);
                    }
                }
            };
            if (isFlagSetInCommandLine("nthreads"))
                set_nthreads_pin("nthreads");

            if (isFlagSetInCommandLine("pin"))
                set_nthreads_pin("pin");

            if (device.find("CPU") != std::string::npos || device.find("GPU") != std::string::npos) {
                // CPU supports few special performance-oriented keys
                // for CPU and GPU execution, more throughput-oriented execution via streams
                setThroughputStreams();
                set_infer_precision();
            } else if (device.find("MYRIAD") != std::string::npos) {
                device_config.emplace(ov::log::level(ov::log::Level::WARNING));
                setThroughputStreams();
            } else if (device.find("GNA") != std::string::npos) {
                set_infer_precision();
            } else if (device.find("AUTO") != std::string::npos) {
                setThroughputStreams();
                set_infer_precision();
                device_nstreams.erase(device);
            } else if (device.find("MULTI") != std::string::npos) {
                setThroughputStreams();
                set_infer_precision();
                if ((device_name.find("GPU") != std::string::npos) && (device_name.find("CPU") != std::string::npos)) {
                    slog::warn << "GPU throttling is turned on. Multi-device execution with "
                                  "the CPU + GPU performs best with GPU throttling hint, "
                               << "which releases another CPU thread (that is otherwise "
                                  "used by the GPU driver for active polling)."
                               << slog::endl;

                    device_config.insert(ov::device::properties("GPU", {{GPU_CONFIG_KEY(PLUGIN_THROTTLE), 1}}));
                    // limit threading for CPU portion of inference
                    if (!isFlagSetInCommandLine("pin")) {
                        auto it_affinity = device_config.find(ov::affinity.name());
                        if (it_affinity != device_config.end()) {
                            slog::warn << "Turn off threads pinning for " << device
                                       << " device since multi-scenario with GPU device is used." << slog::endl;
                            it_affinity->second = ov::Affinity::NONE;
                        }
                    }
                }
                device_nstreams.erase(device);
            }
        }

        for (auto&& item : config) {
            core.set_property(item.first, item.second);
        }

        size_t batchSize = FLAGS_b;
        ov::element::Type type = ov::element::undefined;
        std::string topology_name = "";
        std::vector<benchmark_app::InputsInfo> app_inputs_info;
        std::string output_name;

        // Takes priority over config from file
        if (!FLAGS_cache_dir.empty()) {
            core.set_property(ov::cache_dir(FLAGS_cache_dir));
        }

        // If set batch size, disable the auto batching
        if (FLAGS_b > 0) {
            slog::warn << "Batch size is set. Auto batching will be disabled" << slog::endl;
            core.set_property(ov::hint::allow_auto_batching(false));
        }

        bool isDynamicNetwork = false;

        if (FLAGS_load_from_file && !isNetworkCompiled) {
            if (!FLAGS_mean_values.empty() || !FLAGS_scale_values.empty()) {
                throw std::runtime_error("--mean_values and --scale_values aren't supported with --load_from_file. "
                                         "The values can be set via model_optimizer while generating xml");
            }
            next_step();
            slog::info << "Skipping the step for loading model from file" << slog::endl;
            next_step();
            slog::info << "Skipping the step for loading model from file" << slog::endl;
            next_step();
            slog::info << "Skipping the step for loading model from file" << slog::endl;
            auto startTime = Time::now();
            compiledModel = core.compile_model(FLAGS_m, device_name);
            auto duration_ms = get_duration_ms_till_now(startTime);
            slog::info << "Compile model took " << double_to_string(duration_ms) << " ms" << slog::endl;
            slog::info << "Original model I/O parameters:" << slog::endl;
            printInputAndOutputsInfoShort(compiledModel);

            if (statistics)
                statistics->add_parameters(
                    StatisticsReport::Category::EXECUTION_RESULTS,
                    {StatisticsVariant("compile model time (ms)", "load_model_time", duration_ms)});

            convert_io_names_in_map(inputFiles, compiledModel.inputs());
            app_inputs_info = get_inputs_info(FLAGS_shape,
                                              FLAGS_layout,
                                              batchSize,
                                              FLAGS_data_shape,
                                              inputFiles,
                                              FLAGS_scale_values,
                                              FLAGS_mean_values,
                                              compiledModel.inputs());
            if (batchSize == 0) {
                batchSize = 1;
            }

        } else if (!isNetworkCompiled) {
            // ----------------- 4. Reading the Intermediate Representation network
            // ----------------------------------------
            next_step();

            slog::info << "Loading model files" << slog::endl;

            auto startTime = Time::now();
            auto model = core.read_model(FLAGS_m);
            auto duration_ms = get_duration_ms_till_now(startTime);
            slog::info << "Read model took " << double_to_string(duration_ms) << " ms" << slog::endl;
            slog::info << "Original model I/O parameters:" << slog::endl;
            printInputAndOutputsInfoShort(*model);

            if (statistics)
                statistics->add_parameters(StatisticsReport::Category::EXECUTION_RESULTS,
                                           {StatisticsVariant("read model time (ms)", "read_model_time", duration_ms)});

            const auto& inputInfo = std::const_pointer_cast<const ov::Model>(model)->inputs();
            if (inputInfo.empty()) {
                throw std::logic_error("no inputs info is provided");
            }

            // ----------------- 5. Resizing network to match image sizes and given
            // batch ----------------------------------
            for (auto& item : model->inputs()) {
                if (item.get_tensor().get_names().empty()) {
                    item.get_tensor_ptr()->set_names(
                        std::unordered_set<std::string>{item.get_node_shared_ptr()->get_name()});
                }
            }
            next_step();
            convert_io_names_in_map(inputFiles, std::const_pointer_cast<const ov::Model>(model)->inputs());
            // Parse input shapes if specified
            bool reshape = false;
            app_inputs_info = get_inputs_info(FLAGS_shape,
                                              FLAGS_layout,
                                              FLAGS_b,
                                              FLAGS_data_shape,
                                              inputFiles,
                                              FLAGS_scale_values,
                                              FLAGS_mean_values,
                                              inputInfo,
                                              reshape);
            if (reshape) {
                benchmark_app::PartialShapes shapes = {};
                for (auto& item : app_inputs_info[0])
                    shapes[item.first] = item.second.partialShape;
                slog::info << "Reshaping model: " << get_shapes_string(shapes) << slog::endl;
                startTime = Time::now();
                model->reshape(shapes);
                duration_ms = get_duration_ms_till_now(startTime);
                slog::info << "Reshape model took " << double_to_string(duration_ms) << " ms" << slog::endl;
                if (statistics)
                    statistics->add_parameters(
                        StatisticsReport::Category::EXECUTION_RESULTS,
                        {StatisticsVariant("reshape model time (ms)", "reshape_model_time", duration_ms)});
            }

            // ----------------- 6. Configuring inputs and outputs
            // ----------------------------------------------------------------------
            next_step();
            auto preproc = ov::preprocess::PrePostProcessor(model);

            std::map<std::string, std::string> user_precisions_map;
            if (!FLAGS_iop.empty()) {
                user_precisions_map = parseArgMap(FLAGS_iop);
                convert_io_names_in_map(user_precisions_map,
                                        std::const_pointer_cast<const ov::Model>(model)->inputs(),
                                        std::const_pointer_cast<const ov::Model>(model)->outputs());
            }

            const auto input_precision = FLAGS_ip.empty() ? ov::element::undefined : getPrecision2(FLAGS_ip);
            const auto output_precision = FLAGS_op.empty() ? ov::element::undefined : getPrecision2(FLAGS_op);

            const auto& inputs = model->inputs();
            for (int i = 0; i < inputs.size(); i++) {
                const auto& item = inputs[i];
                auto iop_precision = ov::element::undefined;
                auto type_to_set = ov::element::undefined;
                std::string name;
                try {
                    // Some tensors might have no names, get_any_name will throw exception in that case.
                    // -iop option will not work for those tensors.
                    name = item.get_any_name();
                    iop_precision = getPrecision2(user_precisions_map.at(item.get_any_name()));
                } catch (...) {
                }

                if (iop_precision != ov::element::undefined) {
                    type_to_set = iop_precision;
                } else if (input_precision != ov::element::undefined) {
                    type_to_set = input_precision;
                } else if (!name.empty() && app_inputs_info[0].at(name).is_image()) {
                    // image input, set U8
                    type_to_set = ov::element::u8;
                }

                auto& in = preproc.input(item.get_any_name());
                if (type_to_set != ov::element::undefined) {
                    in.tensor().set_element_type(type_to_set);

                    if (!name.empty()) {
                        for (auto& info : app_inputs_info) {
                            info.at(name).type = type_to_set;
                        }
                    }
                }
                // Explicitly set inputs layout.
                if (!name.empty() && !app_inputs_info[0].at(name).layout.empty()) {
                    in.model().set_layout(app_inputs_info[0].at(name).layout);
                }
            }

            fuse_mean_scale(preproc, app_inputs_info.at(0));

            const auto& outs = model->outputs();
            for (int i = 0; i < outs.size(); i++) {
                const auto& item = outs[i];
                auto iop_precision = ov::element::undefined;
                try {
                    // Some tensors might have no names, get_any_name will throw exception in that case.
                    // -iop option will not work for those tensors.
                    iop_precision = getPrecision2(user_precisions_map.at(item.get_any_name()));
                } catch (...) {
                }

                if (iop_precision != ov::element::undefined) {
                    preproc.output(i).tensor().set_element_type(iop_precision);
                } else if (output_precision != ov::element::undefined) {
                    preproc.output(i).tensor().set_element_type(output_precision);
                }
            }

            model = preproc.build();

            // Check if network has dynamic shapes
            auto input_info = app_inputs_info[0];
            isDynamicNetwork = std::any_of(input_info.begin(),
                                           input_info.end(),
                                           [](const std::pair<std::string, benchmark_app::InputInfo>& i) {
                                               return i.second.partialShape.is_dynamic();
                                           });

            topology_name = model->get_friendly_name();

            batchSize = get_batch_size(app_inputs_info.at(0));
            warn_if_no_batch(app_inputs_info.at(0));
            slog::info << "Model batch size: " << batchSize << slog::endl;

            printInputAndOutputsInfoShort(*model);
            // ----------------- 7. Loading the model to the device
            // --------------------------------------------------------
            next_step();
            startTime = Time::now();
            compiledModel = core.compile_model(model, device_name);
            duration_ms = get_duration_ms_till_now(startTime);
            slog::info << "Compile model took " << double_to_string(duration_ms) << " ms" << slog::endl;
            if (statistics)
                statistics->add_parameters(
                    StatisticsReport::Category::EXECUTION_RESULTS,
                    {StatisticsVariant("compile model time (ms)", "load_model_time", duration_ms)});
        } else {
            if (!FLAGS_mean_values.empty() || !FLAGS_scale_values.empty()) {
                throw std::runtime_error("--mean_values and --scale_values aren't supported for compiled model. "
                                         "The values can be set via model_optimizer while generating xml");
            }
            next_step();
            slog::info << "Skipping the step for compiled model" << slog::endl;
            next_step();
            slog::info << "Skipping the step for compiled model" << slog::endl;
            next_step();
            slog::info << "Skipping the step for compiled model" << slog::endl;
            // ----------------- 7. Loading the model to the device
            // --------------------------------------------------------
            next_step();
            auto startTime = Time::now();

            std::ifstream modelStream(FLAGS_m, std::ios_base::binary | std::ios_base::in);
            if (!modelStream.is_open()) {
                throw std::runtime_error("Cannot open model file " + FLAGS_m);
            }
            compiledModel = core.import_model(modelStream, device_name, {});
            modelStream.close();

            auto duration_ms = get_duration_ms_till_now(startTime);
            slog::info << "Import model took " << double_to_string(duration_ms) << " ms" << slog::endl;
            slog::info << "Original model I/O paramteters:" << slog::endl;
            printInputAndOutputsInfoShort(compiledModel);

            if (statistics)
                statistics->add_parameters(
                    StatisticsReport::Category::EXECUTION_RESULTS,
                    {StatisticsVariant("import model time (ms)", "import_model_time", duration_ms)});

            convert_io_names_in_map(inputFiles, compiledModel.inputs());
            app_inputs_info = get_inputs_info(FLAGS_shape,
                                              FLAGS_layout,
                                              FLAGS_b,
                                              FLAGS_data_shape,
                                              inputFiles,
                                              FLAGS_scale_values,
                                              FLAGS_mean_values,
                                              compiledModel.inputs());
            if (batchSize == 0) {
                batchSize = 1;
            }
        }

        if (isDynamicNetwork && FLAGS_api == "sync") {
            throw std::logic_error("Benchmarking of the model with dynamic shapes is available for async API only. "
                                   "Please use -api async -nstreams 1 -nireq 1 to emulate sync behavior");
        }

        // Defining of benchmark mode
        // for static models inference only mode is used as default one
        bool inferenceOnly = FLAGS_inference_only;
        if (isDynamicNetwork) {
            if (isFlagSetInCommandLine("inference_only") && inferenceOnly && app_inputs_info.size() != 1) {
                throw std::logic_error(
                    "Dynamic models with different input data shapes must be benchmarked only in full mode.");
            }
            inferenceOnly = isFlagSetInCommandLine("inference_only") && inferenceOnly && app_inputs_info.size() == 1;
        }

        // ----------------- 8. Querying optimal runtime parameters
        // -----------------------------------------------------
        next_step();

        // output of the actual settings that the device selected
        auto supported_properties = compiledModel.get_property(ov::supported_properties);
        slog::info << "Model:" << slog::endl;
        for (const auto& cfg : supported_properties) {
            if (cfg == ov::supported_properties)
                continue;
            auto prop = compiledModel.get_property(cfg);
            slog::info << "  " << cfg << ": " << prop.as<std::string>() << slog::endl;
        }

        // Update number of streams
        for (auto&& ds : device_nstreams) {
            try {
                const std::string key = getDeviceTypeFromName(ds.first) + "_THROUGHPUT_STREAMS";
                device_nstreams[ds.first] = core.get_property(ds.first, key).as<std::string>();
            } catch (const ov::Exception&) {
                device_nstreams[ds.first] = core.get_property(ds.first, ov::num_streams.name()).as<std::string>();
            }
        }

        // Number of requests
        uint64_t nireq = FLAGS_nireq;
        if (nireq == 0) {
            if (FLAGS_api == "sync") {
                nireq = 1;
            } else {
                try {
                    nireq = compiledModel.get_property(ov::optimal_number_of_infer_requests);
                } catch (const std::exception& ex) {
                    throw ov::Exception("Every device used with the benchmark_app should support " +
                                        std::string(ov::optimal_number_of_infer_requests.name()) +
                                        " Failed to query the metric for the " + device_name +
                                        " with error: " + ex.what());
                }
            }
        }

        // Iteration limit
        uint64_t niter = FLAGS_niter;
        size_t shape_groups_num = app_inputs_info.size();
        if ((niter > 0) && (FLAGS_api == "async")) {
            if (shape_groups_num > nireq) {
                niter = ((niter + shape_groups_num - 1) / shape_groups_num) * shape_groups_num;
                if (FLAGS_niter != niter) {
                    slog::warn << "Number of iterations was aligned by data shape groups number from " << FLAGS_niter
                               << " to " << niter << " using number of possible input shapes " << shape_groups_num
                               << slog::endl;
                }
            } else {
                niter = ((niter + nireq - 1) / nireq) * nireq;
                if (FLAGS_niter != niter) {
                    slog::warn << "Number of iterations was aligned by request number from " << FLAGS_niter << " to "
                               << niter << " using number of requests " << nireq << slog::endl;
                }
            }
        }

        // Time limit
        uint64_t duration_seconds = 0;
        if (FLAGS_t != 0) {
            // time limit
            duration_seconds = FLAGS_t;
        } else if (FLAGS_niter == 0) {
            // default time limit
            duration_seconds = device_default_device_duration_in_seconds(device_name);
        }
        uint64_t duration_nanoseconds = get_duration_in_nanoseconds(duration_seconds);

        if (statistics) {
            statistics->add_parameters(
                StatisticsReport::Category::RUNTIME_CONFIG,
                StatisticsReport::Parameters(
                    {StatisticsVariant("benchmark mode", "benchmark_mode", inferenceOnly ? "inference only" : "full"),
                     StatisticsVariant("topology", "topology", topology_name),
                     StatisticsVariant("target device", "target_device", device_name),
                     StatisticsVariant("API", "api", FLAGS_api),
                     StatisticsVariant("precision", "precision", type.get_type_name()),
                     StatisticsVariant("batch size", "batch_size", batchSize),
                     StatisticsVariant("number of iterations", "iterations_num", niter),
                     StatisticsVariant("number of parallel infer requests", "nireq", nireq),
                     StatisticsVariant("duration (ms)", "duration", get_duration_in_milliseconds(duration_seconds))}));
            for (auto& nstreams : device_nstreams) {
                std::stringstream ss;
                ss << "number of " << nstreams.first << " streams";

                std::string dev_name = nstreams.first;
                std::transform(dev_name.begin(), dev_name.end(), dev_name.begin(), [](unsigned char c) {
                    return c == ' ' ? '_' : std::tolower(c);
                });

                statistics->add_parameters(StatisticsReport::Category::RUNTIME_CONFIG,
                                           {StatisticsVariant(ss.str(), dev_name + "_streams_num", nstreams.second)});
            }
        }

        // ----------------- 9. Creating infer requests and filling input blobs
        // ----------------------------------------
        next_step();

        InferRequestsQueue inferRequestsQueue(compiledModel, nireq, app_inputs_info.size(), FLAGS_pcseq);

        bool inputHasName = false;
        if (inputFiles.size() > 0) {
            inputHasName = inputFiles.begin()->first != "";
        }
        bool newInputType = isDynamicNetwork || inputHasName;
        // create vector to store remote input blobs buffer
        std::vector<::gpu::BufferType> clInputsBuffer;
        bool useGpuMem = false;

        std::map<std::string, ov::TensorVector> inputsData;
        if (isFlagSetInCommandLine("use_device_mem")) {
            if (device_name.find("GPU") == 0) {
                inputsData = ::gpu::get_remote_input_tensors(inputFiles,
                                                             app_inputs_info,
                                                             compiledModel,
                                                             clInputsBuffer,
                                                             inferRequestsQueue.requests.size());
                useGpuMem = true;
            } else if (device_name.find("CPU") == 0) {
                if (newInputType) {
                    inputsData = get_tensors(inputFiles, app_inputs_info);
                } else {
                    inputsData = get_tensors_static_case(
                        inputFiles.empty() ? std::vector<std::string>{} : inputFiles.begin()->second,
                        batchSize,
                        app_inputs_info[0],
                        nireq);
                }
            } else {
                throw ov::Exception("Requested device doesn't support `use_device_mem` option.");
            }
        } else {
            if (newInputType) {
                inputsData = get_tensors(inputFiles, app_inputs_info);
            } else {
                inputsData = get_tensors_static_case(
                    inputFiles.empty() ? std::vector<std::string>{} : inputFiles.begin()->second,
                    batchSize,
                    app_inputs_info[0],
                    nireq);
            }
        }
        // ----------------- 10. Measuring performance
        // ------------------------------------------------------------------
        size_t iteration = 0;

        std::stringstream ss;
        ss << "Start inference " << FLAGS_api << "hronously";
        if (FLAGS_api == "async") {
            if (!ss.str().empty()) {
                ss << ", ";
            }
            ss << nireq << " inference requests";
            std::stringstream device_ss;
            for (auto& nstreams : device_nstreams) {
                if (!device_ss.str().empty()) {
                    device_ss << ", ";
                }
                device_ss << nstreams.second << " streams for " << nstreams.first;
            }
            if (!device_ss.str().empty()) {
                ss << " using " << device_ss.str();
            }
        }
        ss << ", limits: ";
        if (duration_seconds > 0) {
            ss << get_duration_in_milliseconds(duration_seconds) << " ms duration";
        }
        if (niter != 0) {
            if (duration_seconds > 0) {
                ss << ", ";
            }
            ss << niter << " iterations";
        }

        next_step(ss.str());

        if (inferenceOnly) {
            slog::info << "Benchmarking in inference only mode (inputs filling are not included in measurement loop)."
                       << slog::endl;
        } else {
            slog::info << "Benchmarking in full mode (inputs filling are included in measurement loop)." << slog::endl;
        }

        // copy prepared data straight into inferRequest->getTensor()
        // for inference only mode
        if (inferenceOnly) {
            if (nireq < inputsData.begin()->second.size())
                slog::warn << "Only " << nireq << " test configs will be used." << slog::endl;
            size_t i = 0;
            for (auto& inferRequest : inferRequestsQueue.requests) {
                auto inputs = app_inputs_info[i % app_inputs_info.size()];
                for (auto& item : inputs) {
                    auto inputName = item.first;
                    const auto& inputTensor = inputsData.at(inputName)[i % inputsData.at(inputName).size()];
                    // for remote blobs setTensor is used, they are already allocated on the device
                    if (useGpuMem) {
                        inferRequest->set_tensor(inputName, inputTensor);
                    } else {
                        auto requestTensor = inferRequest->get_tensor(inputName);
                        if (isDynamicNetwork) {
                            requestTensor.set_shape(inputTensor.get_shape());
                        }
                        copy_tensor_data(requestTensor, inputTensor);
                    }
                }

                if (useGpuMem) {
                    auto outputTensors =
                        ::gpu::get_remote_output_tensors(compiledModel, inferRequest->get_output_cl_buffer());
                    for (auto& output : compiledModel.outputs()) {
                        inferRequest->set_tensor(output.get_any_name(), outputTensors[output.get_any_name()]);
                    }
                }
                ++i;
            }
        }

        // warming up - out of scope
        auto inferRequest = inferRequestsQueue.get_idle_request();
        if (!inferRequest) {
            throw ov::Exception("No idle Infer Requests!");
        }

        if (!inferenceOnly) {
            auto inputs = app_inputs_info[0];

            for (auto& item : inputs) {
                auto inputName = item.first;
                const auto& data = inputsData.at(inputName)[0];
                inferRequest->set_tensor(inputName, data);
            }

            if (useGpuMem) {
                auto outputTensors =
                    ::gpu::get_remote_output_tensors(compiledModel, inferRequest->get_output_cl_buffer());
                for (auto& output : compiledModel.outputs()) {
                    inferRequest->set_tensor(output.get_any_name(), outputTensors[output.get_any_name()]);
                }
            }
        }

        if (FLAGS_api == "sync") {
            inferRequest->infer();
        } else {
            inferRequest->start_async();
        }

        inferRequestsQueue.wait_all();

        auto duration_ms = inferRequestsQueue.get_latencies()[0];
        slog::info << "First inference took " << double_to_string(duration_ms) << " ms" << slog::endl;

        if (statistics) {
            statistics->add_parameters(
                StatisticsReport::Category::EXECUTION_RESULTS,
                {StatisticsVariant("first inference time (ms)", "first_inference_time", duration_ms)});
        }
        inferRequestsQueue.reset_times();

        size_t processedFramesN = 0;
        auto startTime = Time::now();
        auto execTime = std::chrono::duration_cast<ns>(Time::now() - startTime).count();

        /** Start inference & calculate performance **/
        /** to align number if iterations to guarantee that last infer requests are
         * executed in the same conditions **/
        while ((niter != 0LL && iteration < niter) ||
               (duration_nanoseconds != 0LL && (uint64_t)execTime < duration_nanoseconds) ||
               (FLAGS_api == "async" && iteration % nireq != 0)) {
            inferRequest = inferRequestsQueue.get_idle_request();
            if (!inferRequest) {
                throw ov::Exception("No idle Infer Requests!");
            }

            if (!inferenceOnly) {
                auto inputs = app_inputs_info[iteration % app_inputs_info.size()];

                if (FLAGS_pcseq) {
                    inferRequest->set_latency_group_id(iteration % app_inputs_info.size());
                }

                if (isDynamicNetwork) {
                    batchSize = get_batch_size(inputs);
                }

                for (auto& item : inputs) {
                    auto inputName = item.first;
                    const auto& data = inputsData.at(inputName)[iteration % inputsData.at(inputName).size()];
                    inferRequest->set_tensor(inputName, data);
                }

                if (useGpuMem) {
                    auto outputTensors =
                        ::gpu::get_remote_output_tensors(compiledModel, inferRequest->get_output_cl_buffer());
                    for (auto& output : compiledModel.outputs()) {
                        inferRequest->set_tensor(output.get_any_name(), outputTensors[output.get_any_name()]);
                    }
                }
            }

            if (FLAGS_api == "sync") {
                inferRequest->infer();
            } else {
                inferRequest->start_async();
            }
            ++iteration;

            execTime = std::chrono::duration_cast<ns>(Time::now() - startTime).count();
            processedFramesN += batchSize;
        }

        // wait the latest inference executions
        inferRequestsQueue.wait_all();

        LatencyMetrics generalLatency(inferRequestsQueue.get_latencies(), "", FLAGS_latency_percentile);
        std::vector<LatencyMetrics> groupLatencies = {};
        if (FLAGS_pcseq && app_inputs_info.size() > 1) {
            const auto& lat_groups = inferRequestsQueue.get_latency_groups();
            for (int i = 0; i < lat_groups.size(); i++) {
                const auto& lats = lat_groups[i];

                std::string data_shapes_string = "";
                for (auto& item : app_inputs_info[i]) {
                    data_shapes_string += item.first + item.second.dataShape.to_string() + ",";
                }
                data_shapes_string =
                    data_shapes_string == "" ? "" : data_shapes_string.substr(0, data_shapes_string.size() - 1);

                groupLatencies.emplace_back(lats, data_shapes_string, FLAGS_latency_percentile);
            }
        }

        double totalDuration = inferRequestsQueue.get_duration_in_milliseconds();
        double fps = 1000.0 * processedFramesN / totalDuration;

        if (statistics) {
            statistics->add_parameters(StatisticsReport::Category::EXECUTION_RESULTS,
                                       {StatisticsVariant("total execution time (ms)", "execution_time", totalDuration),
                                        StatisticsVariant("total number of iterations", "iterations_num", iteration)});
            if (device_name.find("MULTI") == std::string::npos) {
                std::string latency_label;
                if (FLAGS_latency_percentile == 50) {
                    latency_label = "Median latency (ms)";
                } else {
                    latency_label = "latency (" + std::to_string(FLAGS_latency_percentile) + " percentile) (ms)";
                }
                statistics->add_parameters(
                    StatisticsReport::Category::EXECUTION_RESULTS,
                    {StatisticsVariant(latency_label, "latency_median", generalLatency.median_or_percentile),
                     StatisticsVariant("Percentile boundary", "percentile_boundary", FLAGS_latency_percentile),
                     StatisticsVariant("Average latency (ms)", "latency_avg", generalLatency.avg),
                     StatisticsVariant("Min latency (ms)", "latency_min", generalLatency.min),
                     StatisticsVariant("Max latency (ms)", "latency_max", generalLatency.max)});

                if (FLAGS_pcseq && app_inputs_info.size() > 1) {
                    for (size_t i = 0; i < groupLatencies.size(); ++i) {
                        statistics->add_parameters(
                            StatisticsReport::Category::EXECUTION_RESULTS_GROUPPED,
                            {StatisticsVariant("Group Latencies", "group_latencies", groupLatencies[i])});
                    }
                }
            }
            statistics->add_parameters(StatisticsReport::Category::EXECUTION_RESULTS,
                                       {StatisticsVariant("throughput", "throughput", fps)});
        }
        // ----------------- 11. Dumping statistics report
        // -------------------------------------------------------------
        next_step();

        if (!FLAGS_dump_config.empty()) {
            dump_config(FLAGS_dump_config, config);
            slog::info << "OpenVINO Runtime configuration settings were dumped to " << FLAGS_dump_config << slog::endl;
        }

        if (!FLAGS_exec_graph_path.empty()) {
            try {
                ov::serialize(compiledModel.get_runtime_model(), FLAGS_exec_graph_path);
                slog::info << "Executable graph is stored to " << FLAGS_exec_graph_path << slog::endl;
            } catch (const std::exception& ex) {
                slog::err << "Can't get executable graph: " << ex.what() << slog::endl;
            }
        }

        if (perf_counts) {
            std::vector<std::vector<ov::ProfilingInfo>> perfCounts;
            for (size_t ireq = 0; ireq < nireq; ireq++) {
                auto reqPerfCounts = inferRequestsQueue.requests[ireq]->get_performance_counts();
                if (!FLAGS_pcsort.empty()) {
                    slog::info << "Sort performance counts for " << ireq << "-th infer request:" << slog::endl;
                    printPerformanceCountsSort(reqPerfCounts,
                                               std::cout,
                                               getFullDeviceName(core, FLAGS_d),
                                               FLAGS_pcsort,
                                               false);
                } else if (FLAGS_pc) {
                    slog::info << "Performance counts for " << ireq << "-th infer request:" << slog::endl;
                    printPerformanceCounts(reqPerfCounts, std::cout, getFullDeviceName(core, FLAGS_d), false);
                }
                perfCounts.push_back(reqPerfCounts);
            }
            if (statistics) {
                statistics->dump_performance_counters(perfCounts);
            }
        }

        if (statistics)
            statistics->dump();

        // Performance metrics report
        try {
            auto exeDevice = compiledModel.get_property(ov::execution_devices);
            slog::info << "Execution Devices: " << exeDevice << slog::endl;
        } catch (const ov::Exception&) {
        }

        slog::info << "Count:               " << iteration << " iterations" << slog::endl;
        slog::info << "Duration:            " << double_to_string(totalDuration) << " ms" << slog::endl;

        if (device_name.find("MULTI") == std::string::npos) {
            slog::info << "Latency:" << slog::endl;
            generalLatency.write_to_slog();

            if (FLAGS_pcseq && app_inputs_info.size() > 1) {
                slog::info << "Latency for each data shape group:" << slog::endl;
                for (size_t i = 0; i < app_inputs_info.size(); ++i) {
                    slog::info << (i + 1) << ".";
                    for (auto& item : app_inputs_info[i]) {
                        std::stringstream input_shape;
                        auto shape = item.second.dataShape;
                        std::copy(shape.begin(), shape.end() - 1, std::ostream_iterator<size_t>(input_shape, ","));
                        input_shape << shape.back();
                        slog::info << " " << item.first << " : " << item.second.dataShape;
                    }
                    slog::info << slog::endl;

                    groupLatencies[i].write_to_slog();
                }
            }
        }

        slog::info << "Throughput:          " << double_to_string(fps) << " FPS" << slog::endl;

    } catch (const std::exception& ex) {
        slog::err << ex.what() << slog::endl;

        if (statistics) {
            statistics->add_parameters(StatisticsReport::Category::EXECUTION_RESULTS,
                                       {StatisticsVariant("error", "error", ex.what())});
            statistics->dump();
        }

        return 3;
    }

    return 0;
}
