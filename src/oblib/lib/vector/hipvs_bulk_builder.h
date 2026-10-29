/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace oceanbase { namespace common { namespace obvsag {

// Snapshot-only staging. The completed graph is imported into the existing VSAG
// engine, so query filtering, allocator ownership and persistence stay native.
class HipvsBulkBuilder {
public:
  // Explicit environment overrides win; otherwise use only an executable
  // sibling of the running seekdb binary (never search cwd or PATH).
  static std::string find_worker();
  HipvsBulkBuilder(std::string worker, int dim, int degree, int ef_construction)
    : worker_(std::move(worker)), dim_(dim), degree_(degree), ef_construction_(ef_construction) {}
  void append(const float *vectors, const int64_t *ids, int dim, int count);
  std::string build_native_graph() const;
  void release_staging();
  int64_t size() const { return static_cast<int64_t>(ids_.size()); }
  const float *vectors() const { return vectors_.data(); }
  const int64_t *ids() const { return ids_.data(); }
  int dim() const { return dim_; }
  int degree() const { return degree_; }
  bool complete() const { return complete_; }
private:
  std::string worker_;
  int dim_;
  int degree_;
  int ef_construction_;
  bool complete_ = false;
  std::vector<float> vectors_;
  std::vector<int64_t> ids_;
  std::unordered_set<int64_t> seen_;
};

}}}
