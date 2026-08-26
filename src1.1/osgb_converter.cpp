#include "osgb_converter.h"
#include "osg_gltf_converter.h"
#include "hlod_optimizer.h"
#include "coordinate_system.h"
#include "coordinate_transformer.h"
#include "geoid_height.h"
#include "mesh_processor.h"

#include <nlohmann/json.hpp>
#include <ogr_spatialref.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <memory>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <glm/gtc/type_ptr.hpp>

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

// Cesium 1.134 performs a tileset-wide SSE early exit using the top-level
// tileset geometricError before it traverses the root tile.  Keep this outer
// gate deliberately large so zooming out cannot skip the whole tileset; the
// root tile's own geometricError still controls normal refinement.
constexpr double kTopLevelTilesetGeometricError = 1.0e9;
using json = nlohmann::json;

namespace osgb_converter {

// Forward declaration
static std::vector<std::string> split_string(const std::string& s, char delim);

struct Phase1Job {
    std::string osgb_path;
    std::string stem;
    std::string out_tile_dir;
};

struct Phase1TreeResult {
    osg_tree root;
    std::string stem;
    std::string out_tile_dir;
};

struct BadInput {
    std::string path;
    std::string reason;
};

static json tree_to_worker_json(const osg_tree& tree) {
    json value = {
        {"file", tree.file_name},
        {"type", tree.type},
        {"children", json::array()}
    };
    if (!tree.bbox.min.empty()) value["bbox_min"] = tree.bbox.min;
    if (!tree.bbox.max.empty()) value["bbox_max"] = tree.bbox.max;
    for (const auto& child : tree.sub_nodes)
        value["children"].push_back(tree_to_worker_json(child));
    return value;
}

static osg_tree tree_from_worker_json(const json& value) {
    osg_tree tree;
    tree.file_name = value.at("file").get<std::string>();
    tree.type = value.value("type", 1);
    if (value.contains("bbox_min"))
        tree.bbox.min = value.at("bbox_min").get<std::vector<double>>();
    if (value.contains("bbox_max"))
        tree.bbox.max = value.at("bbox_max").get<std::vector<double>>();
    if (value.contains("children")) {
        for (const auto& child : value.at("children"))
            tree.sub_nodes.push_back(tree_from_worker_json(child));
    }
    return tree;
}

#ifndef _WIN32
static bool read_exact_fd(int fd, void* data, size_t size) {
    char* out = static_cast<char*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::read(fd, out + done, size - done);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

static bool write_exact_fd(int fd, const void* data, size_t size) {
    const char* in = static_cast<const char*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, in + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

static bool read_worker_frame(int fd, std::string& payload) {
    uint64_t length = 0;
    if (!read_exact_fd(fd, &length, sizeof(length))) return false;
    constexpr uint64_t kMaxFrameBytes = 1024ull * 1024ull * 1024ull;
    if (length > kMaxFrameBytes) return false;
    payload.resize(static_cast<size_t>(length));
    return length == 0 || read_exact_fd(fd, payload.data(), payload.size());
}

static bool write_worker_frame(int fd, const std::string& payload) {
    const uint64_t length = static_cast<uint64_t>(payload.size());
    return write_exact_fd(fd, &length, sizeof(length))
        && (payload.empty() || write_exact_fd(fd, payload.data(), payload.size()));
}
#endif

int run_phase1_reader_worker() {
#ifdef _WIN32
    return 2;
#else
    constexpr int kResultFd = 3;
    std::string request_text;
    while (read_worker_frame(STDIN_FILENO, request_text)) {
        if (request_text.empty()) break;

        json response;
        std::vector<BadInput> failures;
        try {
            const json request = json::parse(request_text);
            std::string path = request.at("path").get<std::string>();
            TreeFailureCallback on_failure = [&failures](
                const std::string& failed_path, const std::string& reason) {
                failures.push_back({failed_path, reason});
            };

            LOG_I("Phase 1 worker - Building tree: %s", path.c_str());
            osg_tree root = get_all_tree(path, true, on_failure);
            if (root.file_name.empty()) {
                response = {{"ok", false}, {"reason", "failed to build tile tree"}};
            } else {
                response = {{"ok", true}, {"tree", tree_to_worker_json(root)}};
            }
        } catch (const std::exception& e) {
            response = {{"ok", false}, {"reason", e.what()}};
        } catch (...) {
            response = {{"ok", false}, {"reason", "unknown worker exception"}};
        }

        response["failures"] = json::array();
        for (const auto& failure : failures) {
            response["failures"].push_back({
                {"path", failure.path}, {"reason", failure.reason}});
        }
        if (!write_worker_frame(kResultFd, response.dump())) return 3;
    }
    return 0;
#endif
}

#ifndef _WIN32
static bool run_phase1_process_pool(
    const std::vector<Phase1Job>& jobs,
    const ConvertOptions& opts,
    std::vector<Phase1TreeResult>& results,
    std::vector<BadInput>& bad_inputs) {
    if (jobs.empty()) return true;

    using Clock = std::chrono::steady_clock;
    constexpr size_t kLogLimitPerTile = 64u * 1024u;
    constexpr size_t kMaxDrainPerPass = 256u * 1024u;
    constexpr uint64_t kMaxFrameBytes = 1024ull * 1024ull * 1024ull;

    struct Worker {
        pid_t pid = -1;
        int command_fd = -1;
        int result_fd = -1;
        int log_fd = -1;
        bool busy = false;
        size_t job_index = 0;
        Clock::time_point started;
        std::vector<char> result_buffer;
        uint64_t expected_bytes = UINT64_MAX;
        size_t log_forwarded = 0;
        size_t log_dropped = 0;
    };

    auto close_fd = [](int& fd) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    };
    auto set_nonblocking = [](int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    };
    auto spawn_worker = [&](Worker& worker) -> bool {
        int command_pipe[2] = {-1, -1};
        int result_pipe[2] = {-1, -1};
        int log_pipe[2] = {-1, -1};
        if (::pipe(command_pipe) != 0
            || ::pipe(result_pipe) != 0
            || ::pipe(log_pipe) != 0) {
            LOG_E("Failed to create Phase 1 worker pipes: %s", std::strerror(errno));
            for (int fd : command_pipe) if (fd >= 0) ::close(fd);
            for (int fd : result_pipe) if (fd >= 0) ::close(fd);
            for (int fd : log_pipe) if (fd >= 0) ::close(fd);
            return false;
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            LOG_E("Failed to fork Phase 1 worker: %s", std::strerror(errno));
            for (int fd : command_pipe) ::close(fd);
            for (int fd : result_pipe) ::close(fd);
            for (int fd : log_pipe) ::close(fd);
            return false;
        }
        if (pid == 0) {
            char executable_path[PATH_MAX + 1] = {};
            const ssize_t executable_length = ::readlink(
                "/proc/self/exe", executable_path, PATH_MAX);
            if (executable_length > 0)
                executable_path[executable_length] = '\0';
            if (::dup2(command_pipe[0], STDIN_FILENO) < 0
                || ::dup2(log_pipe[1], STDOUT_FILENO) < 0
                || ::dup2(log_pipe[1], STDERR_FILENO) < 0
                || ::dup2(result_pipe[1], 3) < 0) {
                _exit(126);
            }
            for (int fd : command_pipe) if (fd > 3) ::close(fd);
            for (int fd : result_pipe) if (fd > 3) ::close(fd);
            for (int fd : log_pipe) if (fd > 3) ::close(fd);
            ::execl("/proc/self/exe",
                    executable_length > 0 ? executable_path : "/proc/self/exe",
                    "--phase1-reader-worker", static_cast<char*>(nullptr));
            _exit(127);
        }

        ::close(command_pipe[0]);
        ::close(result_pipe[1]);
        ::close(log_pipe[1]);
        worker = Worker{};
        worker.pid = pid;
        worker.command_fd = command_pipe[1];
        worker.result_fd = result_pipe[0];
        worker.log_fd = log_pipe[0];
        set_nonblocking(worker.result_fd);
        set_nonblocking(worker.log_fd);
        return true;
    };

    auto stop_worker = [&](Worker& worker, bool force) {
        close_fd(worker.command_fd);
        if (worker.pid > 0) {
            int status = 0;
            if (force) ::kill(worker.pid, SIGTERM);
            bool exited = false;
            for (int i = 0; i < 25; ++i) {
                const pid_t waited = ::waitpid(worker.pid, &status, WNOHANG);
                if (waited == worker.pid || (waited < 0 && errno == ECHILD)) {
                    exited = true;
                    break;
                }
                ::usleep(20000);
            }
            if (!exited) {
                ::kill(worker.pid, SIGKILL);
                while (::waitpid(worker.pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
        close_fd(worker.result_fd);
        close_fd(worker.log_fd);
        worker.pid = -1;
        worker.busy = false;
    };

    auto assign_job = [&](Worker& worker, size_t job_index) -> bool {
        const json request = {{"path", jobs[job_index].osgb_path}};
        if (!write_worker_frame(worker.command_fd, request.dump())) return false;
        worker.busy = true;
        worker.job_index = job_index;
        worker.started = Clock::now();
        worker.result_buffer.clear();
        worker.expected_bytes = UINT64_MAX;
        worker.log_forwarded = 0;
        worker.log_dropped = 0;
        LOG_I("Phase 1 - Assigned grid to reader pid=%d: %s",
              static_cast<int>(worker.pid), jobs[job_index].osgb_path.c_str());
        return true;
    };

    auto drain_log = [&](Worker& worker) {
        char buffer[8192];
        size_t drained = 0;
        for (;;) {
            const ssize_t n = ::read(worker.log_fd, buffer, sizeof(buffer));
            if (n > 0) {
                drained += static_cast<size_t>(n);
                const size_t available = worker.log_forwarded < kLogLimitPerTile
                    ? kLogLimitPerTile - worker.log_forwarded : 0;
                const size_t forward = std::min(available, static_cast<size_t>(n));
                if (forward > 0) {
                    std::fwrite(buffer, 1, forward, stderr);
                    std::fflush(stderr);
                    worker.log_forwarded += forward;
                }
                worker.log_dropped += static_cast<size_t>(n) - forward;
                if (drained >= kMaxDrainPerPass) break;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
    };

    auto take_response = [&](Worker& worker, std::string& payload) -> int {
        char buffer[65536];
        for (;;) {
            const ssize_t n = ::read(worker.result_fd, buffer, sizeof(buffer));
            if (n > 0) {
                worker.result_buffer.insert(
                    worker.result_buffer.end(), buffer, buffer + n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (worker.expected_bytes == UINT64_MAX
            && worker.result_buffer.size() >= sizeof(uint64_t)) {
            std::memcpy(&worker.expected_bytes,
                        worker.result_buffer.data(), sizeof(uint64_t));
            worker.result_buffer.erase(
                worker.result_buffer.begin(),
                worker.result_buffer.begin() + sizeof(uint64_t));
            if (worker.expected_bytes > kMaxFrameBytes) return -1;
        }
        if (worker.expected_bytes != UINT64_MAX
            && worker.result_buffer.size() >= worker.expected_bytes) {
            payload.assign(worker.result_buffer.data(),
                           static_cast<size_t>(worker.expected_bytes));
            return 1;
        }
        return 0;
    };

    // A dead reader must cause write() to fail instead of terminating the parent.
    ::signal(SIGPIPE, SIG_IGN);

    const size_t worker_count = std::min(
        jobs.size(), static_cast<size_t>(std::max(1, opts.tile_reader_processes)));
    std::vector<Worker> workers(worker_count);
    for (auto& worker : workers) {
        if (!spawn_worker(worker)) {
            for (auto& started : workers)
                if (started.pid > 0) stop_worker(started, true);
            return false;
        }
    }

    size_t next_job = 0;
    size_t completed = 0;
    for (auto& worker : workers) {
        if (next_job < jobs.size() && !assign_job(worker, next_job++)) {
            LOG_E("Failed to send initial job to Phase 1 worker");
            for (auto& started : workers) stop_worker(started, true);
            return false;
        }
    }

    auto record_job_failure = [&](const Worker& worker, const std::string& reason) {
        const auto& job = jobs[worker.job_index];
        bad_inputs.push_back({job.osgb_path, reason});
        LOG_E("Skipping top-level grid [%s]: %s",
              job.osgb_path.c_str(), reason.c_str());
    };

    while (completed < jobs.size()) {
        std::vector<pollfd> poll_fds;
        std::vector<std::pair<size_t, bool>> owners;
        for (size_t i = 0; i < workers.size(); ++i) {
            if (workers[i].pid <= 0) continue;
            poll_fds.push_back({workers[i].result_fd, POLLIN | POLLHUP | POLLERR, 0});
            owners.push_back({i, false});
            poll_fds.push_back({workers[i].log_fd, POLLIN | POLLHUP | POLLERR, 0});
            owners.push_back({i, true});
        }
        if (!poll_fds.empty()) {
            int rc;
            do { rc = ::poll(poll_fds.data(), poll_fds.size(), 100); }
            while (rc < 0 && errno == EINTR);
            if (rc < 0) {
                LOG_E("Phase 1 worker poll failed: %s", std::strerror(errno));
                for (auto& worker : workers) stop_worker(worker, true);
                return false;
            }
            for (size_t p = 0; p < poll_fds.size(); ++p) {
                if (poll_fds[p].revents == 0) continue;
                Worker& worker = workers[owners[p].first];
                if (owners[p].second) drain_log(worker);
            }
        }

        for (auto& worker : workers) {
            if (worker.pid <= 0 || !worker.busy) continue;
            std::string payload;
            const int response_state = take_response(worker, payload);
            if (response_state != 0) {
                drain_log(worker);
                if (worker.log_dropped > 0) {
                    LOG_W("Suppressed %zu bytes of reader output for grid: %s",
                          worker.log_dropped, jobs[worker.job_index].osgb_path.c_str());
                }
                if (response_state < 0) {
                    record_job_failure(worker, "invalid oversized worker response");
                } else {
                    try {
                        const json response = json::parse(payload);
                        if (response.contains("failures")) {
                            for (const auto& failure : response.at("failures")) {
                                bad_inputs.push_back({
                                    failure.value("path", jobs[worker.job_index].osgb_path),
                                    failure.value("reason", "reader failure")});
                            }
                        }
                        if (response.value("ok", false)) {
                            results.push_back({
                                tree_from_worker_json(response.at("tree")),
                                jobs[worker.job_index].stem,
                                jobs[worker.job_index].out_tile_dir});
                        } else {
                            record_job_failure(
                                worker, response.value("reason", "reader failed"));
                        }
                    } catch (const std::exception& e) {
                        record_job_failure(worker,
                            std::string("invalid worker response: ") + e.what());
                    }
                }
                worker.busy = false;
                ++completed;
                if (next_job < jobs.size()) {
                    if (!assign_job(worker, next_job++)) {
                        stop_worker(worker, true);
                        if (!spawn_worker(worker)
                            || !assign_job(worker, next_job - 1)) {
                            LOG_E("Failed to replace Phase 1 reader process");
                            for (auto& other : workers) stop_worker(other, true);
                            return false;
                        }
                    }
                }
                continue;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - worker.started).count();
            if (elapsed >= opts.tile_read_timeout) {
                record_job_failure(worker,
                    "Phase 1 read timeout after " +
                    std::to_string(opts.tile_read_timeout) + " seconds");
                drain_log(worker);
                if (worker.log_dropped > 0) {
                    LOG_W("Suppressed %zu bytes of reader output for timed-out grid: %s",
                          worker.log_dropped, jobs[worker.job_index].osgb_path.c_str());
                }
                stop_worker(worker, true);
                ++completed;
                if (next_job < jobs.size()) {
                    if (!spawn_worker(worker) || !assign_job(worker, next_job++)) {
                        LOG_E("Failed to replace timed-out Phase 1 reader process");
                        for (auto& other : workers) stop_worker(other, true);
                        return false;
                    }
                }
                continue;
            }

            int status = 0;
            const pid_t waited = ::waitpid(worker.pid, &status, WNOHANG);
            if (waited == worker.pid) {
                const std::string reason = WIFSIGNALED(status)
                    ? "reader process crashed with signal " + std::to_string(WTERMSIG(status))
                    : "reader process exited with code " +
                      std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                record_job_failure(worker, reason);
                close_fd(worker.command_fd);
                close_fd(worker.result_fd);
                close_fd(worker.log_fd);
                worker.pid = -1;
                worker.busy = false;
                ++completed;
                if (next_job < jobs.size()) {
                    if (!spawn_worker(worker) || !assign_job(worker, next_job++)) {
                        LOG_E("Failed to replace crashed Phase 1 reader process");
                        for (auto& other : workers) stop_worker(other, true);
                        return false;
                    }
                }
            }
        }
    }

    for (auto& worker : workers) stop_worker(worker, false);
    LOG_I("Phase 1 complete: %zu tile trees built (%zu persistent reader processes, %d-second timeout)",
          results.size(), worker_count, opts.tile_read_timeout);
    return true;
}
#endif

// ============================================================
// metadata.xml parsing
// ============================================================
struct ModelMetadata {
    std::string version;
    std::string SRS;
    std::string SRSOrigin;
};

static bool parse_metadata_xml(const std::string& dir, ModelMetadata& meta) {
    fs::path meta_file = fs::path(dir) / "metadata.xml";
    if (!fs::exists(meta_file)) {
        LOG_E("metadata.xml not found at %s", meta_file.string().c_str());
        return false;
    }

    std::ifstream ifs(meta_file);
    if (!ifs.is_open()) {
        LOG_E("cannot open %s", meta_file.string().c_str());
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    LOG_I("metadata.xml content: %s", content.c_str());

    // Simple XML parsing for SRS and SRSOrigin (manual parsing to avoid XML dependency)
    auto extract_tag = [&](const std::string& tag) -> std::string {
        std::string open = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        auto start = content.find(open);
        if (start == std::string::npos) return "";
        start += open.size();
        auto end = content.find(close, start);
        if (end == std::string::npos) return "";
        return content.substr(start, end - start);
    };

    // Also try self-closing / attribute format
    auto extract_attr = [&](const std::string& tag, const std::string& attr) -> std::string {
        std::string search = "<" + tag + " ";
        auto start = content.find(search);
        if (start == std::string::npos) {
            search = "<" + tag + ">";
            start = content.find(search);
            if (start == std::string::npos) return "";
        }
        auto attrPos = content.find(attr + "=\"", start);
        if (attrPos == std::string::npos || attrPos > content.find(">", start)) return "";
        attrPos += attr.size() + 2;
        auto end = content.find("\"", attrPos);
        if (end == std::string::npos) return "";
        return content.substr(attrPos, end - attrPos);
    };

    meta.SRS = extract_tag("SRS");
    if (meta.SRS.empty()) meta.SRS = extract_attr("ModelMetadata", "SRS");
    meta.SRSOrigin = extract_tag("SRSOrigin");
    if (meta.SRSOrigin.empty()) meta.SRSOrigin = extract_attr("ModelMetadata", "SRSOrigin");

    // Decode XML entities (&quot; → ", &amp; → &, etc.)
    // Critical for WKT SRS which uses quotes around projection names
    auto xml_decode = [](std::string& s) {
        size_t pos;
        while ((pos = s.find("&quot;")) != std::string::npos)
            s.replace(pos, 6, "\"");
        while ((pos = s.find("&amp;")) != std::string::npos)
            s.replace(pos, 5, "&");
        while ((pos = s.find("&lt;")) != std::string::npos)
            s.replace(pos, 4, "<");
        while ((pos = s.find("&gt;")) != std::string::npos)
            s.replace(pos, 4, ">");
    };
    xml_decode(meta.SRS);
    xml_decode(meta.SRSOrigin);

    LOG_I("Parsed metadata: SRS=%s, SRSOrigin=%s", meta.SRS.c_str(), meta.SRSOrigin.c_str());
    return !meta.SRS.empty();
}

// ============================================================
// Coordinate transformer initialization
// ============================================================
static bool init_coordinate_transformer(const ModelMetadata& meta,
                                        double& center_x, double& center_y,
                                        std::optional<std::tuple<double,double,double>>& enu_offset,
                                        std::optional<double>& origin_height,
                                        const std::string& gdal_data, const std::string& proj_lib) {
    // Set GDAL/PROJ paths
    if (!gdal_data.empty()) CPLSetConfigOption("GDAL_DATA", gdal_data.c_str());
    if (!proj_lib.empty()) CPLSetConfigOption("PROJ_LIB", proj_lib.c_str());

    // Verify proj.db accessibility (critical for WKT parsing)
    fprintf(stderr, "[GDAL] GDAL_DATA=%s\n", CPLGetConfigOption("GDAL_DATA", "NOT_SET"));
    fprintf(stderr, "[GDAL] PROJ_LIB=%s\n", CPLGetConfigOption("PROJ_LIB", "NOT_SET"));
    {
        std::string proj_db = proj_lib.empty() ? "" : proj_lib + "/proj.db";
        if (!proj_db.empty() && fs::exists(proj_db)) {
            fprintf(stderr, "[GDAL] proj.db found at: %s\n", proj_db.c_str());
        } else {
            fprintf(stderr, "[GDAL] WARNING: proj.db NOT found at: %s\n", proj_db.c_str());
            fprintf(stderr, "[GDAL] PROJ coordinate transforms will fail!\n");
        }
    }

    // Parse SRS
    std::string srs = meta.SRS;
    auto colon = srs.find(':');
    if (colon == std::string::npos) {
        // Try WKT
        LOG_I("SRS has no colon, treating as WKT");
        std::vector<double> pt = {0, 0, 0};
        auto parts = split_string(meta.SRSOrigin, ',');
        for (size_t i = 0; i < std::min(parts.size(), (size_t)3); i++)
            pt[i] = std::stod(parts[i]);

        auto cs = coords::CoordinateSystem::WKT(srs, pt[0], pt[1], pt[2]);
        auto geo_ref = coords::GeoReference::FromDegrees(0, 0, 0);

        // Create temp OGR transform to get geographic origin
        OGRSpatialReference inRs, outRs;
        inRs.importFromWkt(srs.c_str());
        outRs.importFromEPSG(4326);
        inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
        if (poCT) {
            double lon = pt[0], lat = pt[1], h = pt[2];
            fprintf(stderr, "[WKT] Origin in source CRS: x=%.6f y=%.6f z=%.6f\n", lon, lat, h);
            poCT->Transform(1, &lon, &lat, &h);
            center_x = lon; center_y = lat;
            fprintf(stderr, "[WKT] Origin in WGS84: lon=%.10f lat=%.10f h=%.3f\n", lon, lat, h);
            OGRCoordinateTransformation::DestroyCT(poCT);
        } else {
            fprintf(stderr, "[WKT] ERROR: Failed to create OGR transform (SRS parse failed?)\n");
        }
        geo_ref = coords::GeoReference::FromDegrees(center_x, center_y, pt[2]);

        auto* transformer = new coords::CoordinateTransformer(cs, geo_ref);
        SetGlobalTransformer(transformer);
        origin_height = transformer->GeoOriginHeight();
        LOG_I("WKT init: center=(%.10f, %.10f)", center_x, center_y);
        return true;
    }

    std::string srs_type = srs.substr(0, colon);
    std::string srs_val = srs.substr(colon + 1);

    if (srs_type == "ENU") {
        // Parse ENU: "ENU:lat,lon"
        auto parts = split_string(srs_val, ',');
        if (parts.size() < 2) {
            LOG_E("invalid ENU SRS: %s", srs.c_str());
            return false;
        }
        center_y = std::stod(parts[0]); // lat
        center_x = std::stod(parts[1]); // lon

        // Parse SRSOrigin
        auto origin_parts = split_string(meta.SRSOrigin, ',');
        double ox = 0, oy = 0, oz = 0;
        if (origin_parts.size() >= 2) {
            ox = std::stod(origin_parts[0]);
            oy = std::stod(origin_parts[1]);
            if (origin_parts.size() >= 3) oz = std::stod(origin_parts[2]);
        }

        fprintf(stderr, "[SRS] ENU: %.7f, %.7f (offset: %.3f, %.3f, %.3f)\n",
                center_y, center_x, ox, oy, oz);

        auto cs = coords::CoordinateSystem::ENU(center_x, center_y, 0.0, ox, oy, oz);
        auto* transformer = new coords::CoordinateTransformer(cs);

        SetGlobalTransformer(transformer);
        enu_offset = std::make_tuple(ox, oy, oz);
        origin_height = transformer->GeoOriginHeight();

        LOG_I("ENU init: center=(%.10f, %.10f), offset=(%.3f,%.3f,%.3f)",
              center_x, center_y, ox, oy, oz);
        return true;
    }

    if (srs_type == "EPSG") {
        int epsg_code = std::stoi(srs_val);
        auto origin_parts = split_string(meta.SRSOrigin, ',');
        double ox = 0, oy = 0, oz = 0;
        if (origin_parts.size() >= 2) {
            ox = std::stod(origin_parts[0]);
            oy = std::stod(origin_parts[1]);
            if (origin_parts.size() >= 3) oz = std::stod(origin_parts[2]);
        }

        fprintf(stderr, "[SRS] EPSG:%d -> EPSG:4326\n", epsg_code);
        fprintf(stderr, "[Origin] x=%.6f y=%.6f z=%.3f\n", ox, oy, oz);

        auto cs = coords::CoordinateSystem::EPSG(epsg_code, ox, oy, oz);

        // Create OGR transform to get geographic origin
        OGRSpatialReference inRs, outRs;
        inRs.importFromEPSG(epsg_code);
        outRs.importFromEPSG(4326);
        inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
        if (!poCT) {
            LOG_E("Failed to create OGR transform for EPSG:%d", epsg_code);
            return false;
        }
        double lon = ox, lat = oy, h = oz;
        poCT->Transform(1, &lon, &lat, &h);
        center_x = lon; center_y = lat;
        OGRCoordinateTransformation::DestroyCT(poCT);

        fprintf(stderr, "[Origin LLA] lon=%.10f lat=%.10f h=%.3f\n", lon, lat, h);

        auto geo_ref = coords::GeoReference::FromDegrees(lon, lat, h);

        // Check geoid
        coords::GeoidConfig gc;
        if (GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) {
            gc = coords::GeoidConfig::EGM96();
            gc.enabled = true;
        }
        auto* transformer = new coords::CoordinateTransformer(cs, geo_ref, gc);
        SetGlobalTransformer(transformer);
        origin_height = transformer->GeoOriginHeight();

        LOG_I("EPSG init: center=(%.10f, %.10f), origin_height=%.3f", center_x, center_y, *origin_height);
        return true;
    }

    LOG_E("Unknown SRS type: %s", srs_type.c_str());
    return false;
}

// Simple string split helper
static std::vector<std::string> split_string(const std::string& s, char delim) {
    std::vector<std::string> result;
    size_t start = 0, end;
    while ((end = s.find(delim, start)) != std::string::npos) {
        result.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    if (start < s.size()) result.push_back(s.substr(start));
    return result;
}

// ============================================================
// ENU→ECEF transform matrix computation
// ============================================================
static std::vector<double> transfrom_xyz(double lon_deg, double lat_deg, double height_min) {
    glm::dmat4 mat = coords::CoordinateTransformer::CalcEnuToEcefMatrix(lon_deg, lat_deg, height_min);
    std::vector<double> result(16);
    const double* ptr = glm::value_ptr(mat);
    std::memcpy(result.data(), ptr, 16 * sizeof(double));
    return result;
}

static std::vector<double> box_to_tileset_box(const std::vector<double>& box_v) {
    std::vector<double> box_new;
    box_new.push_back((box_v[0] + box_v[3]) / 2.0);
    box_new.push_back((box_v[1] + box_v[4]) / 2.0);
    box_new.push_back((box_v[2] + box_v[5]) / 2.0);
    box_new.push_back(std::abs(box_v[3] - box_v[0]) / 2.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(std::abs(box_v[4] - box_v[1]) / 2.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(std::abs(box_v[5] - box_v[2]) / 2.0);
    return box_new;
}

// Remove source LOD nodes above max_lvl before conversion and JSON encoding.
// This keeps the generated files and the tileset tree in sync.
static size_t count_tree_nodes(const osg_tree& tree) {
    size_t count = 1;
    for (const auto& child : tree.sub_nodes)
        count += count_tree_nodes(child);
    return count;
}

static size_t prune_above_max_level(osg_tree& tree, int max_lvl) {
    size_t removed = 0;
    auto& children = tree.sub_nodes;
    for (auto it = children.begin(); it != children.end();) {
        const int level = get_lvl_num(it->file_name);
        if (level >= 0 && level > max_lvl) {
            removed += count_tree_nodes(*it);
            it = children.erase(it);
        } else {
            removed += prune_above_max_level(*it, max_lvl);
            ++it;
        }
    }
    return removed;
}

// Return the drawable PagedLOD files at a relative coarse-to-fine depth.
// Depth 0 is the tile-directory root, depth 1 is its next-coarser frontier,
// and so on. Structural group nodes do not consume a LOD depth. If one branch
// ends before the requested depth, retain its finest available drawable so a
// REPLACE refinement can never make that part of the tile disappear.
static void collect_source_lod_paths(
    const osg_tree& tree, int relative_depth, std::vector<std::string>& paths)
{
    if (tree.type == 0) {
        for (const auto& child : tree.sub_nodes)
            collect_source_lod_paths(child, relative_depth, paths);
        return;
    }

    // type=2 is the non-PagedLOD geometry from the same source file. Loading
    // the type=1 OSGB already gives the HLOD merger the intended coarse model,
    // so do not add the same file a second time here.
    if (tree.type != 1) return;

    std::vector<const osg_tree*> next_lod;
    std::function<void(const osg_tree&)> collect_next;
    collect_next = [&](const osg_tree& candidate) {
        if (candidate.type == 1) {
            next_lod.push_back(&candidate);
        } else if (candidate.type == 0) {
            for (const auto& child : candidate.sub_nodes)
                collect_next(child);
        }
    };
    for (const auto& child : tree.sub_nodes)
        collect_next(child);

    if (relative_depth > 0 && !next_lod.empty()) {
        for (const osg_tree* child : next_lod)
            collect_source_lod_paths(*child, relative_depth - 1, paths);
        return;
    }

    auto append_unique = [&paths](const std::string& path) {
        if (!path.empty()
            && std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    };
    if (!tree.aggregate_sources.empty()) {
        for (const auto& source : tree.aggregate_sources)
            append_unique(source);
    } else {
        append_unique(tree.file_name);
    }
}

static void collect_hlod_leaf_stems(
    const QuadNode& node, std::vector<std::string>& stems)
{
    if (node.level == 0) {
        stems.insert(stems.end(), node.leaf_stems.begin(), node.leaf_stems.end());
        return;
    }
    for (const auto& child : node.children)
        collect_hlod_leaf_stems(child, stems);
}

// The spatial hierarchy height determines exactly how many relative source
// LOD frontiers progressive HLOD can consume. This mirrors build_quadtree():
// branching 4 pads to powers of 2, branching 16 to powers of 4, etc.
static int calculate_hlod_source_max_depth(
    const std::vector<Phase1TreeResult>& trees, int branching_factor)
{
    bool initialized = false;
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (const auto& tree : trees) {
        if (!is_valid_tile_box(tree.root.bbox)) continue;
        int x = 0, y = 0;
        if (!parse_tile_grid_coords(tree.stem, x, y)) continue;
        if (!initialized) {
            min_x = max_x = x;
            min_y = max_y = y;
            initialized = true;
        } else {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    if (!initialized) return 0;

    const long long width = static_cast<long long>(max_x) - min_x + 1;
    const long long height = static_cast<long long>(max_y) - min_y + 1;
    const long long max_dimension = std::max(width, height);
    if (max_dimension <= 1) return 0;

    const int branch_side = static_cast<int>(
        std::sqrt(static_cast<double>(branching_factor)));
    long long padded_size = branch_side;
    int root_level = 0;
    while (padded_size < max_dimension) {
        if (padded_size > std::numeric_limits<long long>::max() / branch_side)
            break;
        padded_size *= branch_side;
        ++root_level;
    }
    return root_level;
}

// In progressive (non-Git HEAD) mode, offset 0 preserves the historical
// Root-first sequence. A positive offset means "skip N numbered LODs after
// Root": for Root -> L14 -> L15 -> ... an offset of 5 starts at L19.
static int progressive_source_lod_start_depth(int level_offset) {
    return level_offset > 0 ? level_offset + 1 : 0;
}

static bool collect_finest_leaf_sources(
    const osg_tree& tree,
    size_t max_sources,
    uintmax_t max_input_bytes,
    std::vector<std::string>& sources,
    uintmax_t& input_bytes)
{
    // Keep group/other-geometry nodes out of a PagedLOD aggregate.
    if (tree.type != 1)
        return false;

    if (tree.sub_nodes.empty()) {
        if (tree.file_name.empty() || !tree.aggregate_sources.empty())
            return false;

        std::error_code ec;
        const uintmax_t bytes = fs::file_size(fs::path(tree.file_name), ec);
        if (ec || bytes > max_input_bytes
            || sources.size() + 1 > max_sources
            || input_bytes > max_input_bytes - bytes)
            return false;

        sources.push_back(tree.file_name);
        input_bytes += bytes;
        return true;
    }

    for (const auto& child : tree.sub_nodes) {
        if (!collect_finest_leaf_sources(
                child, max_sources, max_input_bytes, sources, input_bytes))
            return false;
    }
    return true;
}

// Replace the largest qualifying finest-LOD spatial subtree with one
// synthetic aggregate leaf. The current node's coarse content is retained.
static size_t aggregate_fine_lod_subtrees(
    osg_tree& tree,
    const std::string& tile_stem,
    size_t max_sources,
    uintmax_t max_input_bytes,
    size_t& aggregate_counter,
    size_t& aggregated_source_count)
{
    if (tree.type == 1 && !tree.sub_nodes.empty()) {
        std::vector<std::string> sources;
        uintmax_t input_bytes = 0;
        bool eligible = true;
        for (const auto& child : tree.sub_nodes) {
            if (!collect_finest_leaf_sources(
                    child, max_sources, max_input_bytes, sources, input_bytes)) {
                eligible = false;
                break;
            }
        }

        if (eligible && sources.size() >= 2) {
            size_t old_descendants = 0;
            for (const auto& child : tree.sub_nodes)
                old_descendants += count_tree_nodes(child);

            char suffix[64];
            std::snprintf(suffix, sizeof(suffix), "_FINE_MERGE_%05zu.osgb",
                          aggregate_counter++);

            osg_tree aggregate;
            aggregate.type = 1;
            aggregate.file_name =
                (fs::path(get_parent(tree.file_name)) / (tile_stem + suffix)).string();
            aggregate.aggregate_sources = std::move(sources);

            aggregated_source_count += aggregate.aggregate_sources.size();
            tree.sub_nodes.clear();
            tree.sub_nodes.push_back(std::move(aggregate));
            return old_descendants > 0 ? old_descendants - 1 : 0;
        }
    }

    size_t removed = 0;
    for (auto& child : tree.sub_nodes) {
        removed += aggregate_fine_lod_subtrees(
            child, tile_stem, max_sources, max_input_bytes,
            aggregate_counter, aggregated_source_count);
    }
    return removed;
}

// ============================================================
// Main conversion entry point
// ============================================================
int convert_osgb(const ConvertOptions& opts) {
    if (opts.tile_read_timeout > 0 && !opts.skip_bad_tiles) {
        LOG_E("--tile-read-timeout requires --skip-bad-tiles");
        return 1;
    }
    if (opts.tile_read_timeout > 0 && opts.hlod_only) {
        LOG_E("--tile-read-timeout is not supported together with --hlod-only");
        return 1;
    }
#ifdef _WIN32
    if (opts.tile_read_timeout > 0) {
        LOG_E("--tile-read-timeout is currently supported on Linux only");
        return 1;
    }
#endif
    if (opts.hlod_only && !opts.enable_top_reconstruct) {
        LOG_E("hlod_only requires enable_top_reconstruct");
        return 1;
    }
    if (opts.git_head_top_reconstruct_level_offset < 0) {
        LOG_E("--git-head-top-reconstruct-level-offset must be >= 0; got %d",
              opts.git_head_top_reconstruct_level_offset);
        return 1;
    }
    if (opts.git_head_top_reconstruct_level_offset > 0
        && !opts.enable_top_reconstruct) {
        LOG_E("--git-head-top-reconstruct-level-offset requires --enable-top-reconstruct (or --hlod-only)");
        return 1;
    }
    if (opts.enable_top_reconstruct) {
        if (opts.hlod_branching_factor < 4) {
            LOG_E("--hlod-branching-factor must be a perfect square >= 4 (for example 4, 9, or 16); got %d",
                  opts.hlod_branching_factor);
            return 1;
        }
        const int branch_side = static_cast<int>(std::sqrt(opts.hlod_branching_factor));
        if (branch_side * branch_side != opts.hlod_branching_factor) {
            LOG_E("--hlod-branching-factor must be a perfect square >= 4 (for example 4, 9, or 16); got %d",
                  opts.hlod_branching_factor);
            return 1;
        }
        LOG_I("HLOD hierarchy: branching=%d (%dx%d spatial children per node)",
              opts.hlod_branching_factor, branch_side, branch_side);
    }

    if (opts.enable_texture_compress) {
        const bool gpu_active = initialize_ktx2_encoder(
            opts.enable_gpu_texture_compress, opts.gpu_texture_serialize);
        LOG_I("KTX2 encoder: requested=%s, active=%s, quality=%d",
              opts.enable_gpu_texture_compress ? "OpenCL GPU" : "CPU",
              gpu_active ? "OpenCL GPU" : "CPU", opts.ktx2_quality);
    }
    using namespace std::chrono;
    auto tick = high_resolution_clock::now();

    // ============================================================
    // 1. Validate input
    // ============================================================
    fs::path in_dir(opts.input_dir);
    if (!fs::exists(in_dir) || !fs::is_directory(in_dir)) {
        LOG_E("Input directory does not exist: %s", opts.input_dir.c_str());
        return 1;
    }

    fs::path out_dir(opts.output_dir);
    fs::create_directories(out_dir);

    // ============================================================
    // 2. Parse metadata.xml
    // ============================================================
    ModelMetadata metadata;
    double center_x = opts.center_x;
    double center_y = opts.center_y;
    std::optional<std::tuple<double,double,double>> enu_offset;
    std::optional<double> origin_height;

    if (!parse_metadata_xml(opts.input_dir, metadata)) {
        LOG_E("Failed to parse metadata.xml");
        return 1;
    }

    // GDAL/PROJ data paths
    std::string gdal_data, proj_lib;
    // Try to find GDAL/PROJ data relative to executable
    const char* gdal_env = getenv("GDAL_DATA");
    const char* proj_env = getenv("PROJ_LIB");
    if (gdal_env) gdal_data = gdal_env;
    if (proj_env) proj_lib = proj_env;

    // Parse JSON config override
    double cfg_x = 0, cfg_y = 0, cfg_offset = 0;
    int cfg_max_lvl = 100;
    bool has_cfg = false;
    if (!opts.config_json.empty()) {
        try {
            json cfg = json::parse(opts.config_json);
            if (cfg.contains("x")) { cfg_x = cfg["x"].get<double>(); has_cfg = true; }
            if (cfg.contains("y")) { cfg_y = cfg["y"].get<double>(); has_cfg = true; }
            if (cfg.contains("offset")) cfg_offset = cfg["offset"].get<double>();
            if (cfg.contains("max_lvl")) cfg_max_lvl = cfg["max_lvl"].get<int>();
        } catch (...) {
            LOG_E("Failed to parse config JSON: %s", opts.config_json.c_str());
        }
    }

    // ============================================================
    // 3. Initialize coordinate transformer
    // ============================================================
    if (!init_coordinate_transformer(metadata, center_x, center_y, enu_offset, origin_height, gdal_data, proj_lib)) {
        LOG_E("Failed to initialize coordinate transformer");
        return 1;
    }

    // Override with config values if provided
    if (has_cfg) {
        center_x = (opts.center_x != 0) ? opts.center_x : cfg_x;
        center_y = (opts.center_y != 0) ? opts.center_y : cfg_y;
    }

    int max_lvl = (opts.max_lvl != 100) ? opts.max_lvl : cfg_max_lvl;

    LOG_I("Conversion parameters: center=(%.10f, %.10f), max_lvl=%d", center_x, center_y, max_lvl);
    LOG_I("Features: texture_compress=%d, meshopt=%d, draco=%d, unlit=%d, top_reconstruct=%d, git_head_top_reconstruct=%d, git_head_level_offset=%d, hlod_only=%d, fine_merge=%d, skip_bad_tiles=%d, tile_read_timeout=%d, tile_reader_processes=%d",
          opts.enable_texture_compress, opts.enable_meshopt, opts.enable_draco, opts.enable_unlit,
          opts.enable_top_reconstruct, opts.use_git_head_top_reconstruct,
          opts.git_head_top_reconstruct_level_offset, opts.hlod_only, opts.enable_fine_merge,
          opts.skip_bad_tiles, opts.tile_read_timeout, opts.tile_reader_processes);

    // ============================================================
    // 4. Find and convert all tiles
    // ============================================================
    fs::path data_dir = in_dir / "Data";
    if (!fs::exists(data_dir) || !fs::is_directory(data_dir)) {
        LOG_E("Data directory not found: %s", data_dir.string().c_str());
        return 1;
    }

    struct TileResult {
        std::string stem;                      // tile directory name (e.g. "Tile_-001_+050")
        json tree_json;                        // parsed tile tree (with URIs fixed)
        std::vector<double> box_v;
        std::string coarsest_path;             // for root tile reconstruction
    };

    // ============================================================
    // Helper: parallel tile processing with concurrency limiter
    // ============================================================
    class Semaphore {
        std::mutex mtx;
        std::condition_variable cv;
        size_t count;
    public:
        explicit Semaphore(size_t n) : count(n) {}
        void acquire() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return count > 0; });
            --count;
        }
        void release() {
            std::unique_lock<std::mutex> lock(mtx);
            ++count;
            cv.notify_one();
        }
    };

    // Helper: recursively fix content URIs in tile JSON tree
    // so they point to glb files relative to root output directory
    std::function<void(json&, const std::string&)> fix_tile_uris;
    fix_tile_uris = [&fix_tile_uris](json& tile, const std::string& prefix) {
        if (tile.contains("content") && tile["content"].is_object()
            && tile["content"].contains("uri")) {
            std::string uri = tile["content"]["uri"].get<std::string>();
            // uri looks like "./Tile_xxx.glb" — prepend the Data/TileDir/ prefix
            if (uri.size() >= 2 && uri[0] == '.' && (uri[1] == '/' || uri[1] == '\\'))
                tile["content"]["uri"] = "." + prefix + uri.substr(1);
            else
                tile["content"]["uri"] = "." + prefix + uri;
        }
        if (tile.contains("children") && tile["children"].is_array()) {
            for (auto& child : tile["children"])
                fix_tile_uris(child, prefix);
        }
    };

    std::mutex trees_mutex;
    std::vector<Phase1TreeResult> all_trees;

    std::mutex bad_inputs_mutex;
    std::vector<BadInput> bad_inputs;
    TreeFailureCallback record_bad_input =
        [&bad_inputs_mutex, &bad_inputs](const std::string& path,
                                        const std::string& reason) {
            std::lock_guard<std::mutex> lock(bad_inputs_mutex);
            bad_inputs.push_back({path, reason});
        };

    // Collect jobs first, then dispatch through either the legacy thread path
    // or the Linux timeout-safe persistent process pool.
    // Legacy Phase 1 is I/O-bound, so it remains capped at 4 threads.
    std::vector<std::function<void()>> phase1_tasks;
    std::vector<Phase1Job> phase1_jobs;

    for (auto& entry : fs::directory_iterator(data_dir)) {
        if (!entry.is_directory()) continue;

        fs::path tile_dir = entry.path();
        std::string stem = tile_dir.filename().string();
        fs::path osgb_file = tile_dir / (stem + ".osgb");

        if (!fs::exists(osgb_file) || fs::is_directory(osgb_file)) {
            LOG_E("No OSGB file in tile dir: %s", tile_dir.string().c_str());
            if (opts.skip_bad_tiles)
                record_bad_input(osgb_file.string(), "top-level OSGB file missing");
            continue;
        }

        fs::path out_tile_dir = out_dir / "Data" / stem;
        if (!opts.hlod_only) fs::create_directories(out_tile_dir);

        phase1_jobs.push_back({
            osgb_file.string(), stem, out_tile_dir.string()});

        phase1_tasks.push_back([&trees_mutex, &all_trees, &opts,
                                record_bad_input,
                                osgb_path = osgb_file.string(),
                                stem,
                                out_path = out_tile_dir.string()]() {
            try {
                osg_tree root;
                if (opts.hlod_only) {
                    // First pass: probe only the coarsest root for corrected
                    // bounds. Once all successful roots are known, their actual
                    // spatial hierarchy height determines a bounded tree scan.
                    LOG_I("Phase 1 HLOD-only - Probing coarsest source: %s",
                          osgb_path.c_str());
                    std::string probe_glb;
                    MeshInfo minfo;
                    const bool ok = osgb2glb_buf(
                        osgb_path, probe_glb, minfo, 1,
                        false, false, false, opts.enable_unlit,
                        1.0, opts.draco_pos_bits, opts.draco_normal_bits,
                        opts.draco_uv_bits, opts.ktx2_quality);
                    TileBox bbox;
                    bbox.min = minfo.min;
                    bbox.max = minfo.max;
                    if (!ok || !is_valid_tile_box(bbox)) {
                        LOG_E("Failed to load HLOD source bounds: %s", osgb_path.c_str());
                        if (opts.skip_bad_tiles)
                            record_bad_input(osgb_path, "failed to load HLOD source bounds");
                        return;
                    }
                    root.file_name = osgb_path;
                    root.type = 1;
                    root.bbox = std::move(bbox);
                } else {
                    LOG_I("Phase 1 - Building tree: %s", osgb_path.c_str());
                    std::string path_copy = osgb_path;
                    root = get_all_tree(
                        path_copy, opts.skip_bad_tiles, record_bad_input);
                }
                if (root.file_name.empty()) {
                    LOG_E("Failed to read: %s", osgb_path.c_str());
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(trees_mutex);
                    all_trees.push_back({std::move(root), stem, out_path});
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& e) {
                if (!opts.skip_bad_tiles) throw;
                LOG_E("Skipping top-level grid [%s]: %s", osgb_path.c_str(), e.what());
                record_bad_input(osgb_path, e.what());
                return;
            } catch (...) {
                if (!opts.skip_bad_tiles) throw;
                LOG_E("Skipping top-level grid [%s]: unknown exception", osgb_path.c_str());
                record_bad_input(osgb_path, "unknown exception");
                return;
            }
        });
    }

    if (opts.tile_read_timeout > 0) {
#ifndef _WIN32
        if (!run_phase1_process_pool(
                phase1_jobs, opts, all_trees, bad_inputs)) {
            LOG_E("Phase 1 reader process pool failed");
            return 1;
        }
#endif
    } else if (opts.enable_parallel) {
        unsigned int n_threads = opts.num_threads > 0
            ? (unsigned int)opts.num_threads
            : std::thread::hardware_concurrency();
        unsigned int p1_threads = std::min(n_threads, 4u);  // I/O bound
        Semaphore sem(p1_threads);
        std::vector<std::future<void>> futures;
        futures.reserve(phase1_tasks.size());

        for (auto& task : phase1_tasks) {
            // sem.acquire() INSIDE the lambda — non-blocking submission (Fix #4)
            auto wrapper = [&sem, task = std::move(task)]() {
                sem.acquire();
                try {
                    task();
                } catch (...) {
                    sem.release();
                    throw;
                }
                sem.release();
            };
            futures.push_back(std::async(std::launch::async, std::move(wrapper)));
        }
        for (auto& f : futures) f.get();
        LOG_I(opts.hlod_only
                  ? "Phase 1 HLOD-only complete: %zu coarsest sources loaded (parallel, %u I/O threads)"
                  : "Phase 1 complete: %zu tile trees built (parallel, %u I/O threads)",
              all_trees.size(), p1_threads);
    } else {
        for (auto& task : phase1_tasks) task();
        LOG_I(opts.hlod_only
                  ? "Phase 1 HLOD-only complete: %zu coarsest sources loaded (serial)"
                  : "Phase 1 complete: %zu tile trees built (serial)",
              all_trees.size());
    }

    if (all_trees.empty()) {
        LOG_E("No tile trees were built");
        return 1;
    }

    // HLOD-only does not emit the ordinary detail tree. Discover only the
    // relative source levels that the actual spatial HLOD can consume, rather
    // than recursively opening every finest PagedLOD descendant.
    if (opts.hlod_only && !opts.use_git_head_top_reconstruct) {
        const int spatial_source_max_depth = calculate_hlod_source_max_depth(
            all_trees, opts.hlod_branching_factor);
        const int source_start_depth = progressive_source_lod_start_depth(
            opts.git_head_top_reconstruct_level_offset);
        const int source_max_depth = source_start_depth + spatial_source_max_depth;
        LOG_I("Phase 1 HLOD-only bounded scan: %d spatial HLOD levels consume source depths %d..%d (offset=%d)",
              spatial_source_max_depth + 1, source_start_depth, source_max_depth,
              opts.git_head_top_reconstruct_level_offset);

        if (source_max_depth > 0) {
            auto scan_one_tree = [&](size_t index) {
                auto& result = all_trees[index];
                const TileBox root_bbox = result.root.bbox;
                std::string path_copy = result.root.file_name;
                osg_tree bounded_tree = get_all_tree(
                    path_copy, opts.skip_bad_tiles, record_bad_input,
                    source_max_depth);
                if (bounded_tree.file_name.empty()) {
                    LOG_E("Failed bounded HLOD source scan: %s", path_copy.c_str());
                    result.root = {};
                    return;
                }
                bounded_tree.bbox = root_bbox;
                result.root = std::move(bounded_tree);
            };

            if (opts.enable_parallel) {
                unsigned int n_threads = opts.num_threads > 0
                    ? static_cast<unsigned int>(opts.num_threads)
                    : std::thread::hardware_concurrency();
                const unsigned int scan_threads = std::max(
                    1u, std::min(n_threads, 4u));
                Semaphore sem(scan_threads);
                std::vector<std::future<void>> futures;
                futures.reserve(all_trees.size());
                for (size_t index = 0; index < all_trees.size(); ++index) {
                    futures.push_back(std::async(
                        std::launch::async, [&sem, &scan_one_tree, index]() {
                            sem.acquire();
                            try {
                                scan_one_tree(index);
                            } catch (...) {
                                sem.release();
                                throw;
                            }
                            sem.release();
                        }));
                }
                for (auto& future : futures) future.get();
            } else {
                for (size_t index = 0; index < all_trees.size(); ++index)
                    scan_one_tree(index);
            }

            all_trees.erase(
                std::remove_if(
                    all_trees.begin(), all_trees.end(),
                    [](const Phase1TreeResult& result) {
                        return result.root.file_name.empty();
                    }),
                all_trees.end());
        }

        size_t discovered_nodes = 0;
        for (const auto& tree : all_trees)
            discovered_nodes += count_tree_nodes(tree.root);
        LOG_I("Phase 1 HLOD-only bounded scan complete: %zu source nodes retained across %zu tile directories",
              discovered_nodes, all_trees.size());
    }

    if (opts.skip_bad_tiles) {
        std::sort(bad_inputs.begin(), bad_inputs.end(),
                  [](const BadInput& a, const BadInput& b) {
                      return a.path < b.path
                          || (a.path == b.path && a.reason < b.reason);
                  });
        bad_inputs.erase(
            std::unique(bad_inputs.begin(), bad_inputs.end(),
                        [](const BadInput& a, const BadInput& b) {
                            return a.path == b.path && a.reason == b.reason;
                        }),
            bad_inputs.end());

        const fs::path report_path = out_dir / "failed_tiles.txt";
        if (bad_inputs.empty()) {
            std::error_code ec;
            fs::remove(report_path, ec);
        } else {
            std::ofstream report(report_path);
            report << "# Skipped malformed or missing OSGB inputs\n";
            for (const auto& failure : bad_inputs) {
                std::string reason = failure.reason;
                std::replace(reason.begin(), reason.end(), '\n', ' ');
                std::replace(reason.begin(), reason.end(), '\r', ' ');
                report << failure.path << '\t' << reason << '\n';
            }
            report.close();
            LOG_W("Skipped %zu malformed/missing OSGB inputs; report: %s",
                  bad_inputs.size(), report_path.string().c_str());
        }
    }

    if (all_trees.empty()) {
        LOG_E("No usable tile trees remained after HLOD source scanning");
        return 1;
    }

    // Apply the source-level maximum before flattening. Doing this here keeps
    // Phase 2 conversion and Phase 3 JSON encoding on the exact same tree.
    size_t max_level_pruned = 0;
    for (auto& tr : all_trees) {
        max_level_pruned += prune_above_max_level(tr.root, max_lvl);
    }
    LOG_I("Tree pruning: max_lvl removed=%zu", max_level_pruned);

    size_t fine_merge_pruned = 0;
    size_t fine_merge_count = 0;
    size_t fine_merge_sources = 0;
    if (opts.enable_fine_merge && !opts.hlod_only) {
        const uintmax_t max_bytes =
            static_cast<uintmax_t>(opts.fine_merge_max_input_mb) * 1024u * 1024u;
        for (auto& tr : all_trees) {
            fine_merge_pruned += aggregate_fine_lod_subtrees(
                tr.root, tr.stem,
                static_cast<size_t>(opts.fine_merge_max_sources), max_bytes,
                fine_merge_count, fine_merge_sources);
        }
    }
    LOG_I("Fine merge: aggregates=%zu, sources=%zu, removed tree nodes=%zu (max_sources=%d, max_input_mb=%d, enabled=%d)",
          fine_merge_count, fine_merge_sources, fine_merge_pruned,
          opts.fine_merge_max_sources, opts.fine_merge_max_input_mb,
          opts.enable_fine_merge && !opts.hlod_only);

    // ============================================================
    // Phase 2: Flatten all trees → tile conversion (parallel or serial)
    //           Uses cached_node from Phase 1 to avoid redundant
    //           osgDB::readNodeFiles() calls and OSG global mutex.
    //           Tiles are chunked; compute and I/O are separated:
    //           parallel CPU work first, then serialized file writes.
    // ============================================================
    std::vector<FlatTile> all_tiles;
    {
        auto t0 = std::chrono::steady_clock::now();
        if (!opts.hlod_only) {
            for (auto& tr : all_trees) {
                collect_flat_tiles(tr.root, tr.out_tile_dir, all_tiles);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        LOG_I("Phase 2: Flattened %zu tiles in %.0fms (%s)", all_tiles.size(),
              std::chrono::duration<double, std::milli>(t1 - t0).count(),
              opts.enable_parallel ? "parallel (chunked, serial write)" : "serial");
    }

    if (opts.hlod_only) {
        LOG_I("Phase 2 HLOD-only: source LOD trees discovered; no detail GLB writes");
    } else if (opts.enable_parallel) {
        const size_t CHUNK_SIZE = 16;
        size_t num_chunks = (all_tiles.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;

        unsigned int p2_threads = opts.num_threads > 0
            ? (unsigned int)opts.num_threads
            : std::thread::hardware_concurrency();
        Semaphore sem(p2_threads);
        std::mutex write_mutex;
        std::atomic<size_t> tiles_done{0};
        std::atomic<size_t> tiles_failed{0};
        std::atomic<size_t> active_chunks{0};
        std::vector<std::future<void>> futures;
        futures.reserve(num_chunks);

        auto t_launch_start = std::chrono::steady_clock::now();
        for (size_t ci = 0; ci < num_chunks; ci++) {
            size_t start = ci * CHUNK_SIZE;
            size_t end = std::min(start + CHUNK_SIZE, all_tiles.size());
            size_t chunk_idx = ci;

            auto chunk_tiles = std::make_shared<std::vector<FlatTile>>(
                all_tiles.begin() + start, all_tiles.begin() + end);

            auto task = [&sem, &opts, chunk_tiles, chunk_idx, num_chunks,
                         total_tiles = all_tiles.size(),
                         &write_mutex, &tiles_done,
                         &tiles_failed,
                         &active_chunks,
                         first_flag = std::make_shared<std::atomic<bool>>(false)]() {
                sem.acquire();

                // Log the very first chunk to start
                bool expect = false;
                if (first_flag->compare_exchange_strong(expect, true))
                    LOG_I("Phase 2: First chunk started computing (thread is alive)");

                size_t cur_active = active_chunks.fetch_add(1) + 1;
                LOG_I("  Chunk %zu/%zu [%zu tiles] computing... (active=%zu)",
                      chunk_idx + 1, num_chunks, chunk_tiles->size(), cur_active);

                // === Phase 2a: Parallel computation ===
                size_t chunk_size = chunk_tiles->size();
                auto t0 = std::chrono::steady_clock::now();

                struct ChunkResult {
                    std::string glb_buf;
                    std::string out_file;
                    MeshInfo minfo;
                    osg_tree* tree;
                    bool ok;
                };
                std::vector<ChunkResult> results;
                results.reserve(chunk_size);

                for (const auto& tile : *chunk_tiles) {
                    ChunkResult r;
                    r.tree = tile.tree;
                    if (r.tree) r.tree->content_written = false;
                    r.ok = compute_tile_output(tile, opts, r.glb_buf, r.minfo, r.out_file);
                    results.push_back(std::move(r));
                }

                auto t1 = std::chrono::steady_clock::now();
                double compute_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

                sem.release();

                // === Phase 2b: Serialized file writes ===
                auto t2 = std::chrono::steady_clock::now();
                double wait_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                {
                    std::lock_guard<std::mutex> lock(write_mutex);
                    for (auto& r : results) {
                        const bool written = r.ok && !r.glb_buf.empty()
                            && write_file(r.out_file.c_str(), r.glb_buf.data(),
                                          (unsigned long)r.glb_buf.size());
                        if (r.tree) {
                            r.tree->content_written = written;
                            if (written) {
                                r.tree->bbox.max = r.minfo.max;
                                r.tree->bbox.min = r.minfo.min;
                            }
                        }
                        if (!written) {
                            tiles_failed.fetch_add(1);
                            LOG_E("  GLB conversion/write failed; JSON content will be omitted: %s",
                                  r.out_file.empty() ? "<no output path>" : r.out_file.c_str());
                        }
                    }
                }

                auto t3 = std::chrono::steady_clock::now();
                double write_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

                LOG_I("  Chunk %zu/%zu: compute=%.0fms, write_wait=%.0fms, write=%.0fms",
                      chunk_idx + 1, num_chunks, compute_ms, wait_ms, write_ms);

                size_t done = tiles_done.fetch_add(chunk_size) + chunk_size;
                if (done % 200 < chunk_size || done >= total_tiles)
                    LOG_I("  Progress: %zu/%zu tiles done", done, total_tiles);
            };
            futures.push_back(std::async(std::launch::async, std::move(task)));
        }
        auto t_launch_end = std::chrono::steady_clock::now();
        LOG_I("Phase 2: %zu futures launched in %.0fms, waiting for completion...",
              futures.size(),
              std::chrono::duration<double, std::milli>(t_launch_end - t_launch_start).count());
        for (auto& f : futures) f.get();
        LOG_I("Phase 2 write audit: success=%zu, failed=%zu",
              all_tiles.size() - tiles_failed.load(), tiles_failed.load());
    } else {
        size_t tiles_failed = 0;
        for (const auto& tile : all_tiles) {
            if (!convert_one_tile_from_cached(tile, opts)) {
                ++tiles_failed;
                LOG_E("  GLB conversion/write failed; JSON content will be omitted: %s",
                      tile.file_name.c_str());
            }
        }
        LOG_I("Phase 2 write audit: success=%zu, failed=%zu",
              all_tiles.size() - tiles_failed, tiles_failed);
    }

    if (!opts.hlod_only)
        LOG_I("Phase 2 complete: %zu tiles converted", all_tiles.size());

    // ============================================================
    // Phase 3: Serial aggregation — bbox, geometricError, tile JSON
    //           Must be serial because extend_tile_box()/calc_geometric_error()
    //           are bottom-up on each tree.
    // ============================================================
    std::vector<TileResult> tile_results;
    std::map<std::string, json> tile_jsons_map;  // stem → tree_json (for HLOD leaves)

    for (auto& tr : all_trees) {
        extend_tile_box(tr.root);
        if (!is_valid_tile_box(tr.root.bbox)) {
            LOG_E("Invalid bounding box for: %s", tr.stem.c_str());
            continue;
        }

        calc_geometric_error(tr.root);
        if (opts.hlod_only) {
            // Descendant bounds are intentionally not converted/probed in this
            // mode, so derive the source-cell baseline from the valid root box.
            tr.root.geometricError = get_geometric_error(tr.root.bbox);
        }

        if (opts.hlod_only) {
            std::vector<double> box_full;
            box_full.insert(box_full.end(), tr.root.bbox.max.begin(), tr.root.bbox.max.end());
            box_full.insert(box_full.end(), tr.root.bbox.min.begin(), tr.root.bbox.min.end());
            tile_results.push_back({tr.stem, json(), box_full, find_coarsest_node(tr.root)});
            continue;
        }

        std::string json_str = encode_tile_json_1_1(tr.root, degree2rad(center_x), degree2rad(center_y));
        if (json_str.empty()) {
            LOG_E("No successfully written GLB in tile tree: %s", tr.stem.c_str());
            continue;
        }
        json tile_tree = json::parse(json_str);

        std::string uri_prefix = "/Data/" + tr.stem;
        fix_tile_uris(tile_tree, uri_prefix);

        tr.root.bbox.extend(0.2);
        std::vector<double> box_full;
        box_full.insert(box_full.end(), tr.root.bbox.max.begin(), tr.root.bbox.max.end());
        box_full.insert(box_full.end(), tr.root.bbox.min.begin(), tr.root.bbox.min.end());

        std::string coarsest_path = find_coarsest_node(tr.root);

        tile_results.push_back({tr.stem, tile_tree, box_full, coarsest_path});
        tile_jsons_map[tr.stem] = tile_tree;  // For HLOD leaf lookup
    }

    LOG_I("Phase 3 complete: %zu tile trees aggregated", tile_results.size());

    if (tile_results.empty()) {
        LOG_E(opts.hlod_only ? "No source tiles were usable for HLOD" : "No tiles were converted");
        return 1;
    }

    // ============================================================
    // Phase 4: Multi-level spatial HLOD construction (if enabled)
    //           Each top-down HLOD depth consumes the matching coarse-to-fine
    //           source frontier: root=coarsest, child=next-coarsest, ...
    // ============================================================
    QuadNode quadtree_root;
    bool has_hlod = false;
    if (opts.enable_top_reconstruct) {
        std::vector<std::string> tile_stems;
        std::vector<std::string> coarsest_paths;
        std::vector<TileBox> tile_bboxes;
        std::vector<double> coarsest_ges;

        for (auto& tr : all_trees) {
            if (!is_valid_tile_box(tr.root.bbox)) continue;
            tile_stems.push_back(tr.stem);

            double coarsest_ge = 0.0;
            coarsest_paths.push_back(find_coarsest_node(tr.root, &coarsest_ge));
            coarsest_ges.push_back(coarsest_ge);
            TileBox bbox;
            bbox.max = tr.root.bbox.max;
            bbox.min = tr.root.bbox.min;
            tile_bboxes.push_back(bbox);
        }

        if (!tile_stems.empty()) {
            SpatialGrid grid = build_spatial_grid(
                tile_stems, coarsest_paths, tile_bboxes, coarsest_ges);
            quadtree_root = build_quadtree(grid, opts.hlod_branching_factor);
            has_hlod = quadtree_root.has_content || !quadtree_root.children.empty();

            if (has_hlod) {
                fs::path hlod_dir = out_dir / "Data" / "HLOD";
                std::error_code ec;
                fs::create_directories(hlod_dir, ec);

                std::function<void(QuadNode&)> merge_node;
                const int root_level = quadtree_root.level;
                std::map<std::string, const osg_tree*> source_trees;
                if (!opts.use_git_head_top_reconstruct) {
                    for (const auto& tr : all_trees)
                        source_trees[tr.stem] = &tr.root;
                }

                merge_node = [&](QuadNode& node) {
                    for (auto& child : node.children) {
                        merge_node(child);
                    }

                    std::vector<std::string> source_paths;
                    std::vector<HlodIntermediatePtr> child_intermediates;
                    if (opts.use_git_head_top_reconstruct) {
                        if (node.level == 0) {
                            source_paths = node.leaf_coarsest_paths;
                        } else {
                            for (auto& child : node.children) {
                                if (child.intermediate) {
                                    child_intermediates.push_back(child.intermediate);
                                } else {
                                    collect_leaf_paths(child, grid, source_paths);
                                }
                            }
                        }
                    } else {
                        std::vector<std::string> node_stems;
                        collect_hlod_leaf_stems(node, node_stems);
                        node.source_lod_depth = progressive_source_lod_start_depth(
                            opts.git_head_top_reconstruct_level_offset)
                            + root_level - node.level;
                        for (const auto& stem : node_stems) {
                            auto source_it = source_trees.find(stem);
                            if (source_it == source_trees.end()) {
                                LOG_W("HLOD merge: source tree not found for %s", stem.c_str());
                                continue;
                            }
                            collect_source_lod_paths(
                                *source_it->second, node.source_lod_depth, source_paths);
                        }
                    }

                    if (source_paths.empty() && child_intermediates.empty()) {
                        LOG_W("HLOD merge: level %d node at (%d,%d) has no inputs, skipping",
                              node.level, node.grid_x, node.grid_y);
                        node.has_content = false;
                        return;
                    }

                    std::string glb_name;
                    if (node.level == root_level) {
                        glb_name = "root.glb";
                    } else {
                        char buf[128];
                        const int display_level = root_level - 1 - node.level;
                        snprintf(buf, sizeof(buf), "L%d_X%+04d_Y%+04d.glb",
                                 display_level, node.grid_x, node.grid_y);
                        glb_name = buf;
                    }

                    if (opts.use_git_head_top_reconstruct) {
                        LOG_I("HLOD merge [Git HEAD]: %s (level=%d, branching=%d, %zu OSGB fallbacks, %zu direct child models, grid=(%d,%d) size=%d)",
                              glb_name.c_str(), node.level, opts.hlod_branching_factor,
                              source_paths.size(), child_intermediates.size(),
                              node.grid_x, node.grid_y, node.grid_size);
                    } else {
                        LOG_I("HLOD merge: %s (spatial-level=%d, source-lod-depth=%d, branching=%d, %zu source OSGBs, grid=(%d,%d) size=%d)",
                              glb_name.c_str(), node.level, node.source_lod_depth,
                              opts.hlod_branching_factor, source_paths.size(),
                              node.grid_x, node.grid_y, node.grid_size);
                    }

                    std::string glb_buf;
                    TileBox merged_bbox;
                    const int hlod_level = node.level + 1
                        + (opts.use_git_head_top_reconstruct
                            ? opts.git_head_top_reconstruct_level_offset
                            : 0);
                    bool ok = build_merged_glb(
                        source_paths, hlod_level, glb_buf, merged_bbox,
                        opts.enable_texture_compress, opts.enable_meshopt,
                        opts.enable_draco, opts.enable_unlit,
                        opts.top_texture_max_size, opts.simplify_ratio,
                        opts.draco_pos_bits, opts.draco_normal_bits, opts.draco_uv_bits,
                        opts.ktx2_quality, child_intermediates,
                        opts.use_git_head_top_reconstruct ? &node.intermediate : nullptr,
                        opts.hlod_branching_factor);

                    // The carrier GLB above is deliberately unsimplified and
                    // uncompressed. Merge all of its primitives and textures
                    // first, then run the original level simplification,
                    // texture compression and Draco exactly once.
                    if (ok && !glb_buf.empty()) {
                        const double target_ratio = opts.enable_meshopt
                            ? calc_level_ratio(hlod_level, opts.simplify_ratio,
                                               opts.hlod_branching_factor)
                            : 1.0;
                        const int atlas_cell_size = calc_hlod_texture_max_size(
                            hlod_level, opts.top_texture_max_size,
                            opts.hlod_branching_factor);
                        std::string optimized_glb;
                        std::string optimize_error;
                        if (!optimize_hlod_glb_buffer(
                                glb_buf, optimized_glb, atlas_cell_size,
                                target_ratio, opts.enable_texture_compress,
                                opts.ktx2_quality, opts.draco_pos_bits,
                                opts.draco_normal_bits, opts.draco_uv_bits,
                                &optimize_error)) {
                            LOG_E("HLOD one-primitive optimization failed for %s: %s",
                                  glb_name.c_str(), optimize_error.c_str());
                            ok = false;
                            glb_buf.clear();
                        } else {
                            LOG_I("HLOD finalized once: %s, one primitive, atlas-cell=%d, ratio=%.4f, Draco=%d/%d/%d, %zu -> %zu bytes",
                                  glb_name.c_str(), atlas_cell_size, target_ratio,
                                  opts.draco_pos_bits, opts.draco_normal_bits,
                                  opts.draco_uv_bits, glb_buf.size(), optimized_glb.size());
                            glb_buf.swap(optimized_glb);
                        }
                    }

                    if (opts.use_git_head_top_reconstruct) {
                        for (auto& child : node.children)
                            child.intermediate.reset();
                    }

                    if (ok && !glb_buf.empty()) {
                        fs::path glb_path = hlod_dir / glb_name;
                        if (write_file(glb_path.string().c_str(), glb_buf.data(),
                                       (unsigned long)glb_buf.size())) {
                            node.bbox = merged_bbox;
                            node.glb_uri = "./Data/HLOD/" + glb_name;
                            node.has_content = true;
                            LOG_I("  written: %s (%zu bytes)",
                                  glb_path.string().c_str(), glb_buf.size());
                        } else {
                            LOG_E("  failed to write: %s", glb_path.string().c_str());
                            node.has_content = false;
                            node.glb_uri.clear();
                        }
                    } else {
                        LOG_W("  merge failed for level %d node at (%d,%d)",
                              node.level, node.grid_x, node.grid_y);
                        node.has_content = false;
                        node.glb_uri.clear();
                        if (opts.use_git_head_top_reconstruct)
                            node.intermediate.reset();
                    }
                };

                merge_node(quadtree_root);

                if (!quadtree_root.has_content || quadtree_root.glb_uri.empty()) {
                    const bool has_fallback_descendants =
                        !quadtree_root.children.empty() || !quadtree_root.leaf_stems.empty();
                    if (!has_fallback_descendants) {
                        LOG_E("HLOD root has neither drawable content nor descendants");
                        return 1;
                    }
                    LOG_W("HLOD root is index-only; Cesium will refine to spatial descendants");
                }

                LOG_I("Phase 4 complete: %d-ary HLOD tree built (%d levels)",
                      opts.hlod_branching_factor, root_level + 1);
            } else {
                LOG_W("Spatial HLOD build returned empty root, falling back to flat tileset");
            }
        } else {
            LOG_W("No tile stems collected, skipping HLOD build");
        }
    }

    if (opts.hlod_only && !has_hlod) {
        LOG_E("HLOD-only mode did not produce an HLOD hierarchy (at least two spatial source tiles are required)");
        return 1;
    }

    // ============================================================
    // Helper: recursively rewrite content URIs in a tile JSON tree
    // for external sub-tilesets. Since sub-tilesets live in
    // ./subtilesets/ (one level below root), all "./Data/..." URIs
    // need to become "../Data/..." so they resolve correctly.
    // ============================================================
    std::function<void(json&)> rewrite_uris_for_sub_tileset;
    rewrite_uris_for_sub_tileset = [&rewrite_uris_for_sub_tileset](json& tile) {
        if (tile.contains("content") && tile["content"].is_object()
            && tile["content"].contains("uri")) {
            std::string uri = tile["content"]["uri"].get<std::string>();
            // Replace "./Data/" → "../Data/" so URIs resolve from subtilesets/
            if (uri.size() >= 2 && uri[0] == '.' && uri[1] == '/') {
                tile["content"]["uri"] = ".." + uri.substr(1);
            }
        }
        if (tile.contains("children") && tile["children"].is_array()) {
            for (auto& child : tile["children"])
                rewrite_uris_for_sub_tileset(child);
        }
    };

    // ============================================================
    // 5. Build tileset.json
    // ============================================================
    // Compute root bounding box (from tile_results for flat mode, from quadtree for HLOD)
    std::vector<double> root_box = {-1e38, -1e38, -1e38, 1e38, 1e38, 1e38};
    double root_ge = 0.0;

    if (has_hlod) {
        // Use quadtree root bbox
        for (int i = 0; i < 3; i++) {
            root_box[i] = quadtree_root.bbox.max[i];
            root_box[i+3] = quadtree_root.bbox.min[i];
        }
        root_ge = quadtree_root.geometricError;
    } else {
        for (auto& tr : tile_results) {
            for (int i = 0; i < 3; i++) {
                if (tr.box_v[i] > root_box[i]) root_box[i] = tr.box_v[i];
            }
            for (int i = 3; i < 6; i++) {
                if (tr.box_v[i] < root_box[i]) root_box[i] = tr.box_v[i];
            }
            if (tr.tree_json.contains("geometricError"))
                root_ge = std::max(root_ge, tr.tree_json["geometricError"].get<double>());
        }
    }

    // Compute transform matrix
    double trans_height = 0.0;
    if (origin_height.has_value()) {
        trans_height = *origin_height;
    } else if (enu_offset.has_value()) {
        trans_height = std::get<2>(*enu_offset);
    } else if (opts.has_region_offset) {
        trans_height = opts.region_offset - root_box[5];
    }

    std::vector<double> trans_vec = transfrom_xyz(center_x, center_y, trans_height);

    // Apply ENU offset to translation if applicable
    if (enu_offset.has_value()) {
        double eox = std::get<0>(*enu_offset), eoy = std::get<1>(*enu_offset), eoz = std::get<2>(*enu_offset);
        double lat_rad = degree2rad(center_y), lon_rad = degree2rad(center_x);
        double sinLat = std::sin(lat_rad), cosLat = std::cos(lat_rad);
        double sinLon = std::sin(lon_rad), cosLon = std::cos(lon_rad);

        double ecx = -sinLon * eox - sinLat * cosLon * eoy + cosLat * cosLon * eoz;
        double ecy =  cosLon * eox - sinLat * sinLon * eoy + cosLat * sinLon * eoz;
        double ecz =  cosLat * eoy + sinLat * eoz;

        trans_vec[12] += ecx;
        trans_vec[13] += ecy;
        trans_vec[14] += ecz;
    }

    fprintf(stderr, "[transform] lon=%.10f lat=%.10f h=%.3f -> ECEF: x=%.10f y=%.10f z=%.10f\n",
            center_x, center_y, trans_height, trans_vec[12], trans_vec[13], trans_vec[14]);

    // ============================================================
    // Build tileset.json
    // ============================================================
    json root_tileset;
    root_tileset["asset"]["version"] = "1.1";
    root_tileset["asset"]["gltfUpAxis"] = "Z";
    root_tileset["extensionsUsed"] = json::array({"3DTILES_content_gltf"});
    root_tileset["extensionsRequired"] = json::array({"3DTILES_content_gltf"});
    root_tileset["geometricError"] = kTopLevelTilesetGeometricError;

    if (has_hlod) {
        const std::string out_dir_str =
            opts.enable_split_json ? out_dir.string() : "";
        json quadtree_json = encode_quadtree_json(
            quadtree_root, opts.hlod_only ? std::map<std::string, json>{} : tile_jsons_map,
            out_dir_str,
            opts.enable_split_json,
            opts.use_git_head_top_reconstruct);

        // Add ECEF transform to root
        quadtree_json["transform"] = trans_vec;
        quadtree_json["boundingVolume"]["box"] = box_to_tileset_box(root_box);
        root_tileset["root"] = quadtree_json;
    } else {
        // Flat mode (no HLOD)
        if (opts.enable_split_json) {
            // --- Split mode: one external sub-tileset per top-level tree ---
            fs::path sub_dir = out_dir / "subtilesets";
            fs::create_directories(sub_dir);

            json root_tile;
            root_tile["transform"] = trans_vec;
            root_tile["boundingVolume"]["box"] = box_to_tileset_box(root_box);
            root_tile["refine"] = "REPLACE";
            root_tile["geometricError"] = std::min(root_ge, 2000.0);
            root_tile["children"] = json::array();

            for (auto& tr : tile_results) {
                // Deep-copy the tree JSON so we can rewrite URIs without
                // affecting the HLOD leaf lookup (tile_jsons_map).
                json tree_copy = tr.tree_json;
                rewrite_uris_for_sub_tileset(tree_copy);

                // Build sub-tileset envelope
                json sub_tileset;
                sub_tileset["asset"]["version"] = "1.1";
                sub_tileset["extensionsUsed"] = json::array({"3DTILES_content_gltf"});
                sub_tileset["extensionsRequired"] = json::array({"3DTILES_content_gltf"});
                sub_tileset["geometricError"] = tree_copy.value("geometricError", 0.0);
                sub_tileset["root"] = tree_copy;

                // Write subtilesets/<stem>.json
                std::string stem = tr.stem;
                fs::path sub_path = sub_dir / (stem + ".json");
                std::ofstream sub_ofs(sub_path);
                sub_ofs << sub_tileset.dump();
                sub_ofs.close();
                LOG_I("  Wrote sub-tileset: subtilesets/%s.json", stem.c_str());

                // Build lightweight reference tile for root tileset.json
                json ref_tile;
                ref_tile["boundingVolume"]["box"] = box_to_tileset_box(tr.box_v);
                ref_tile["content"]["uri"] = "./subtilesets/" + stem + ".json";
                ref_tile["geometricError"] = tr.tree_json.value("geometricError", 0.0);
                ref_tile["refine"] = "REPLACE";
                root_tile["children"].push_back(ref_tile);
            }

            root_tileset["root"] = root_tile;
        } else {
            // --- Original monolithic mode ---
            json root_tile;
            root_tile["transform"] = trans_vec;
            root_tile["boundingVolume"]["box"] = box_to_tileset_box(root_box);
            root_tile["refine"] = "REPLACE";
            root_tile["geometricError"] = std::min(root_ge, 2000.0);
            root_tile["children"] = json::array();

            for (auto& tr : tile_results) {
                root_tile["children"].push_back(tr.tree_json);
            }

            root_tileset["root"] = root_tile;
        }
    }

    fs::path root_json_path = out_dir / "tileset.json";
    std::ofstream root_ofs(root_json_path);
    root_ofs << root_tileset.dump();
    root_ofs.close();

    // Cleanup
    auto* t = GetGlobalTransformer();
    if (t) { delete t; SetGlobalTransformer(nullptr); }

    auto elapsed = duration_cast<duration<double>>(high_resolution_clock::now() - tick);
    LOG_I("Conversion complete. Time: %.2f s", elapsed.count());
    return 0;
}

} // namespace osgb_converter
