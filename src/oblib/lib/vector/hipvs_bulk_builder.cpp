/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "hipvs_bulk_builder.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
extern char **environ;
#endif

namespace oceanbase { namespace common { namespace obvsag {

std::string HipvsBulkBuilder::find_worker()
{
  const char *override_path = std::getenv("SEEKDB_HIPVS_BUILD_WORKER");
  if (override_path != nullptr) return override_path; // Empty explicitly disables.
#if defined(__linux__)
  char executable[4096];
  const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(executable)) return {};
  std::string path(executable, static_cast<size_t>(length));
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) return {};
  path.resize(slash + 1);
  path += "seekdb-hipvs-hnsw-build";
  struct stat info{};
  if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && access(path.c_str(), X_OK) == 0)
    return path;
#endif
  return {};
}

void HipvsBulkBuilder::append(const float *vectors, const int64_t *ids, int dim, int count)
{
  if (complete_ || !vectors || !ids || count < 0 || dim != dim_ || dim <= 0)
    throw std::invalid_argument("invalid hipVS bulk input");
  if (static_cast<uint64_t>(size()) + count > std::numeric_limits<int>::max())
    throw std::length_error("snapshot count exceeds the native adapter's int range");
  std::unordered_set<int64_t> batch_ids;
  for (int row = 0; row < count; ++row) {
    if (seen_.find(ids[row]) != seen_.end() || (count > 1 && !batch_ids.insert(ids[row]).second))
      throw std::invalid_argument("duplicate snapshot vector ID");
    const float *begin = vectors + static_cast<size_t>(row) * dim;
    for (int d = 0; d < dim; ++d)
      if (!std::isfinite(begin[d])) throw std::invalid_argument("nonfinite snapshot vector");
  }
  // Snapshot ingestion often arrives one row at a time. Reserve geometrically;
  // exact reserve(size+1) would copy the entire snapshot on every input row.
  const size_t floats_needed = vectors_.size() + static_cast<size_t>(count) * dim;
  const size_t ids_needed = ids_.size() + count;
  if (floats_needed > vectors_.capacity())
    vectors_.reserve(std::max(floats_needed, vectors_.capacity() + vectors_.capacity() / 2));
  if (ids_needed > ids_.capacity())
    ids_.reserve(std::max(ids_needed, ids_.capacity() + ids_.capacity() / 2));
  for (int row = 0; row < count; ++row) {
    if (!seen_.insert(ids[row]).second) throw std::invalid_argument("duplicate input batch ID");
    ids_.push_back(ids[row]);
    const float *begin = vectors + static_cast<size_t>(row) * dim;
    vectors_.insert(vectors_.end(), begin, begin + dim);
  }
}

void HipvsBulkBuilder::release_staging()
{
  complete_ = true;
  std::vector<float>().swap(vectors_);
  std::vector<int64_t>().swap(ids_);
  std::unordered_set<int64_t>().swap(seen_);
}

namespace {
template<class T> T read_value(const std::string &bytes, size_t offset)
{
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("truncated hipVS HNSW output");
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}
}

std::string HipvsBulkBuilder::build_native_graph() const
{
#if defined(__linux__)
  if (worker_.empty() || worker_[0] != '/' || dim_ <= 0 || size() < 256 || degree_ < 4 || degree_ > 128)
    throw std::invalid_argument("unsupported hipVS bulk build configuration");
  using File = std::unique_ptr<FILE, int(*)(FILE*)>;
  File input(std::tmpfile(), std::fclose), output(std::tmpfile(), std::fclose);
  if (!input || !output) throw std::runtime_error("cannot create private hipVS IPC files");
  // Files are unlinked by tmpfile; only this child receives the descriptors.
  fcntl(fileno(input.get()), F_SETFD, FD_CLOEXEC);
  fcntl(fileno(output.get()), F_SETFD, FD_CLOEXEC);
  uint64_t header[6] = {0x31444c4955425048ULL, static_cast<uint64_t>(size()),
                       static_cast<uint64_t>(dim_), static_cast<uint64_t>(degree_),
                       static_cast<uint64_t>(ef_construction_), 8};
  if (fwrite(header, sizeof(header), 1, input.get()) != 1 ||
      fwrite(vectors_.data(), sizeof(float), vectors_.size(), input.get()) != vectors_.size() ||
      fflush(input.get()) != 0 || fseek(input.get(), 0, SEEK_SET) != 0)
    throw std::runtime_error("failed to write hipVS build input");
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("spawn actions failed");
  int rc = posix_spawn_file_actions_adddup2(&actions, fileno(input.get()), STDIN_FILENO);
  if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, fileno(output.get()), STDOUT_FILENO);
  char *args[] = {const_cast<char *>(worker_.c_str()), nullptr};
  pid_t pid = -1;
  if (rc == 0) rc = posix_spawn(&pid, worker_.c_str(), &actions, nullptr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) throw std::runtime_error("cannot launch hipVS build worker: " + std::to_string(rc));
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  for (;;) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (done < 0 && errno != EINTR) throw std::runtime_error("hipVS worker wait failed");
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("hipVS build worker timed out");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("hipVS build worker failed, status=" + std::to_string(status));
  if (fseek(output.get(), 0, SEEK_END) != 0) throw std::runtime_error("cannot size hipVS output");
  const long length = ftell(output.get());
  const uint64_t upper_bound = 4096 + static_cast<uint64_t>(size()) * (static_cast<uint64_t>(dim_) * 4 + degree_ * 64 + 128);
  if (length < 96 || static_cast<uint64_t>(length) > upper_bound)
    throw std::runtime_error("unexpected hipVS output size");
  rewind(output.get());
  std::string bytes(static_cast<size_t>(length), '\0');
  if (fread(&bytes[0], 1, bytes.size(), output.get()) != bytes.size())
    throw std::runtime_error("cannot read hipVS output");

  // Validate the supported x86_64, full-hierarchy hnswlib layout before invoking
  // VSAG's native reader. Graph topology stays intact; only external labels change.
  const uint64_t count = read_value<uint64_t>(bytes, 16);
  const uint64_t stride = read_value<uint64_t>(bytes, 24);
  const uint64_t label = read_value<uint64_t>(bytes, 32);
  const uint64_t data = read_value<uint64_t>(bytes, 40);
  const uint64_t max_m = read_value<uint64_t>(bytes, 56);
  const uint64_t max_m0 = read_value<uint64_t>(bytes, 64);
  const uint64_t degree = read_value<uint64_t>(bytes, 72);
  if (read_value<uint64_t>(bytes, 0) != 0 || count != ids_.size() || max_m != static_cast<uint64_t>(degree_) ||
      degree != max_m || max_m0 != 2 * max_m || data != 4 + 4 * max_m0 ||
      label != data + static_cast<uint64_t>(dim_) * sizeof(float) || stride != label + sizeof(int64_t) ||
      count > (bytes.size() - 96) / stride || read_value<uint32_t>(bytes, 52) >= count)
    throw std::runtime_error("incompatible hipVS HNSW graph layout");
  std::vector<bool> seen(count, false);
  for (uint64_t row = 0; row < count; ++row) {
    const size_t pos = 96 + row * stride;
    const uint64_t original = read_value<uint64_t>(bytes, pos + label);
    if (original >= count || seen[original]) throw std::runtime_error("invalid hipVS row-label permutation");
    seen[original] = true;
    if (std::memcmp(bytes.data() + pos + data, vectors_.data() + original * dim_, dim_ * sizeof(float)) != 0)
      throw std::runtime_error("hipVS vector and label mapping mismatch");
    const uint32_t neighbors = read_value<uint32_t>(bytes, pos);
    if (neighbors > max_m0) throw std::runtime_error("invalid hipVS base degree");
    for (uint32_t j = 0; j < neighbors; ++j)
      if (read_value<uint32_t>(bytes, pos + 4 + j * 4) >= count) throw std::runtime_error("invalid hipVS edge");
    std::memcpy(&bytes[pos + label], &ids_[original], sizeof(int64_t));
  }
  size_t pos = 96 + count * stride;
  const size_t level_stride = 4 + max_m * 4;
  for (uint64_t row = 0; row < count; ++row) {
    const uint32_t length = read_value<uint32_t>(bytes, pos); pos += 4;
    if (length % level_stride != 0 || length > bytes.size() - pos)
      throw std::runtime_error("invalid hipVS upper-layer size");
    for (size_t level = 0; level < length; level += level_stride) {
      const uint32_t neighbors = read_value<uint32_t>(bytes, pos + level);
      if (neighbors > max_m) throw std::runtime_error("invalid hipVS upper degree");
      for (uint32_t j = 0; j < neighbors; ++j)
        if (read_value<uint32_t>(bytes, pos + level + 4 + j * 4) >= count) throw std::runtime_error("invalid hipVS upper edge");
    }
    pos += length;
  }
  if (pos != bytes.size()) throw std::runtime_error("unexpected hipVS graph trailing bytes");
  return bytes;
#else
  throw std::runtime_error("hipVS bulk worker requires Linux");
#endif
}

}}}
