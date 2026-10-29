/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "data_plane/fts/ob_fts_struct.h"

#include <atomic>

#include "lib/charset/ob_charset.h"
#include "share/datum/ob_datum_funcs.h"
#include "storage/ob_storage_util.h"

namespace oceanbase
{
namespace storage
{
namespace
{
// ObCharset::get_charset() re-validates the collation and may probe the lazy
// initialization table on every call, which is too expensive for the per-token
// path. The charset objects are stable once initialized, so cache the handle.
inline const ObCharsetInfo *get_collation_info(const ObCollationType cs_type)
{
  static std::atomic<const ObCharsetInfo *> cached[CS_TYPE_MAX];
  const ObCharsetInfo *cs = cached[cs_type].load(std::memory_order_relaxed);
  if (OB_ISNULL(cs)) {
    cs = ObCharset::get_charset(cs_type);
    cached[cs_type].store(cs, std::memory_order_relaxed);
  }
  return cs;
}
} // namespace

// Same results as the datum string hash/compare functions (the datum layer always
// hashes and compares strings with end space ignored), with the collation handle
// cached instead of being resolved through ObDatumFuncs on every token.
int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  const ObCharsetInfo *cs = get_collation_info(meta_.get_collation_type());
  if (OB_ISNULL(cs) || OB_ISNULL(cs->coll) || OB_ISNULL(cs->coll->hash_sort)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(word_.is_null())) {
    common::ObDatumBasicFuncs *funcs = ObDatumFuncs::get_basic_func(meta_.get_type(), meta_.get_collation_type());
    if (OB_ISNULL(funcs) || OB_ISNULL(funcs->default_hash_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ret = funcs->default_hash_(word_, 0, hash_val, nullptr);
    }
  } else {
    // Keep the initial state of ObCharset::hash() so that the values are unchanged.
    ulong nr1 = 0;
    ulong nr2 = 0xc6a4a7935bd1e995;
    cs->coll->hash_sort(cs,
                        reinterpret_cast<const unsigned char *>(word_.ptr_),
                        word_.len_,
                        &nr1,
                        &nr2,
                        false,
                        nullptr);
    hash_val = nr1;
  }
  return ret;
}
bool ObFTWord::operator==(const ObFTWord &other) const
{
  bool is_equal = false;
  const ObCharsetInfo *cs = get_collation_info(meta_.get_collation_type());
  if (OB_NOT_NULL(cs) && OB_NOT_NULL(cs->coll) && OB_NOT_NULL(cs->coll->strnncollsp)
      && !word_.is_null() && !other.word_.is_null()) {
    is_equal = 0 == cs->coll->strnncollsp(
                          cs,
                          reinterpret_cast<const unsigned char *>(word_.ptr_), word_.len_,
                          reinterpret_cast<const unsigned char *>(other.word_.ptr_), other.word_.len_,
                          false);
  } else {
    int ret = OB_SUCCESS;
    int cmp_ret = 0;
    ObDatumCmpFuncType func = get_datum_cmp_func(meta_, other.meta_);
    if (func == nullptr) {
      ob_abort();
    } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
      ob_abort();
    } else {
      is_equal = (cmp_ret == 0);
    }
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
