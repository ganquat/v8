// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMMON_CODE_MEMORY_ACCESS_INL_H_
#define V8_COMMON_CODE_MEMORY_ACCESS_INL_H_

#include "src/common/code-memory-access.h"
// Include the non-inl header before the rest of the headers.

#include "src/flags/flags.h"
#include "src/objects/instruction-stream.h"
#include "src/objects/slots-inl.h"
#include "src/objects/tagged.h"
#if V8_HAS_PKU_JIT_WRITE_PROTECT
#include "src/base/platform/memory-protection-key.h"
#endif
#if V8_HAS_PTHREAD_JIT_WRITE_PROTECT
#include "src/base/platform/platform.h"
#endif
#if V8_HAS_BECORE_JIT_WRITE_PROTECT
#include <BrowserEngineCore/BEMemory.h>
#endif

namespace v8 {
namespace internal {

RwxMemoryWriteScope::RwxMemoryWriteScope(const char* comment) {
  if (!v8_flags.jitless || v8_flags.force_memory_protection_keys) {
    SetWritable();
  }
}

RwxMemoryWriteScope::~RwxMemoryWriteScope() {
  if (!v8_flags.jitless || v8_flags.force_memory_protection_keys) {
    SetExecutable();
  }
}

WritableJitAllocation::~WritableJitAllocation() {
#ifdef DEBUG
  if (enforce_write_api_ && page_ref_.has_value()) {
    // We disabled RWX write access for debugging. But we'll need it in the
    // destructor again to release the jit page reference.
    write_scope_.emplace("~WritableJitAllocation");
  }
#endif
}

WritableJitAllocation::WritableJitAllocation(
    Address addr, size_t size, ThreadIsolation::JitAllocationType type,
    JitAllocationSource source, bool enforce_write_api)
    : address_(addr),
      // The order of these is important. We need to create the write scope
      // before we lookup the Jit page, since the latter will take a mutex in
      // protected memory.
      write_scope_("WritableJitAllocation"),
      page_ref_(ThreadIsolation::LookupJitPage(addr, size)),
      allocation_(source == JitAllocationSource::kRegister
                      ? page_ref_->RegisterAllocation(addr, size, type)
                      : page_ref_->LookupAllocation(addr, size, type)),
      enforce_write_api_(enforce_write_api) {
#ifdef DEBUG
  if (enforce_write_api_) {
    // Reset the write scope for debugging. We'll create fine-grained scopes in
    // all Write functions of this class instead.
    write_scope_.reset();
  }
#else
  // Suppress -Wunused-private-field warning.
  (void)enforce_write_api_;
#endif
}

WritableJitAllocation::WritableJitAllocation(
    Address addr, size_t size, ThreadIsolation::JitAllocationType type,
    bool enforce_write_api)
    : address_(addr),
      allocation_(size, type),
      enforce_write_api_(enforce_write_api) {}

// static
WritableJitAllocation WritableJitAllocation::ForNonExecutableMemory(
    Address addr, size_t size, ThreadIsolation::JitAllocationType type) {
  return WritableJitAllocation(addr, size, type, false);
}

std::optional<RwxMemoryWriteScope>
WritableJitAllocation::WriteScopeForApiEnforcement() const {
#ifdef DEBUG
  if (enforce_write_api_) {
    return std::optional<RwxMemoryWriteScope>("WriteScopeForApiEnforcement");
  }
#endif
  return {};
}

#ifdef V8_ENABLE_WEBASSEMBLY

WritableJumpTablePair::WritableJumpTablePair(Address jump_table_address,
                                             size_t jump_table_size,
                                             Address far_jump_table_address,
                                             size_t far_jump_table_size)
    : writable_jump_table_(jump_table_address, jump_table_size,
                           ThreadIsolation::JitAllocationType::kWasmJumpTable,
                           true),
      writable_far_jump_table_(
          far_jump_table_address, far_jump_table_size,
          ThreadIsolation::JitAllocationType::kWasmFarJumpTable, true),
      write_scope_("WritableJumpTablePair"),
      // Always split the pages since we are not guaranteed that the jump table
      // and far jump table are on the same JitPage.
      jump_table_pages_(ThreadIsolation::SplitJitPages(
          far_jump_table_address, far_jump_table_size, jump_table_address,
          jump_table_size)) {
  CHECK(jump_table_pages_.value().second.Contains(
      jump_table_address, jump_table_size,
      ThreadIsolation::JitAllocationType::kWasmJumpTable));
  CHECK(jump_table_pages_.value().first.Contains(
      far_jump_table_address, far_jump_table_size,
      ThreadIsolation::JitAllocationType::kWasmFarJumpTable));

#ifdef DEBUG
  // Reset the write scope for debugging. We'll create fine-grained scopes in
  // all Write functions of this class instead.
  write_scope_.SetExecutable();
#endif
}

#endif

template <typename T, size_t offset>
void WritableJitAllocation::WriteHeaderSlot(T value) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  // This assert is no strict requirement, it just guards against
  // non-implemented functionality.
  static_assert(!is_taggable_v<T>);

  if constexpr (offset == HeapObject::kMapOffset) {
    TaggedField<T, offset>::Relaxed_Store_Map_Word(
        HeapObject::FromAddress(address_), value);
  } else {
    WriteMaybeUnalignedValue<T>(address_ + offset, value);
  }
}

template <typename T, size_t offset>
void WritableJitAllocation::WriteHeaderSlot(Tagged<T> value, ReleaseStoreTag) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  // These asserts are no strict requirements, they just guard against
  // non-implemented functionality.
  static_assert(offset != HeapObject::kMapOffset);

  TaggedField<T, offset>::Release_Store(HeapObject::FromAddress(address_),
                                        value);
}

template <typename T, size_t offset>
void WritableJitAllocation::WriteHeaderSlot(Tagged<T> value, RelaxedStoreTag) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  if constexpr (offset == HeapObject::kMapOffset) {
    TaggedField<T, offset>::Relaxed_Store_Map_Word(
        HeapObject::FromAddress(address_), value);
  } else {
    TaggedField<T, offset>::Relaxed_Store(HeapObject::FromAddress(address_),
                                          value);
  }
}

template <typename T, size_t offset>
void WritableJitAllocation::WriteProtectedPointerHeaderSlot(Tagged<T> value,
                                                            RelaxedStoreTag) {
  static_assert(offset != HeapObject::kMapOffset);
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  TaggedField<T, offset, TrustedSpaceCompressionScheme>::Relaxed_Store(
      HeapObject::FromAddress(address_), value);
}

template <typename T, size_t offset>
void WritableJitAllocation::WriteProtectedPointerHeaderSlot(Tagged<T> value,
                                                            ReleaseStoreTag) {
  static_assert(offset != HeapObject::kMapOffset);
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  TaggedField<T, offset, TrustedSpaceCompressionScheme>::Release_Store(
      HeapObject::FromAddress(address_), value);
}

template <typename T>
V8_INLINE void WritableJitAllocation::WriteHeaderSlot(Address address, T value,
                                                      RelaxedStoreTag tag) {
  CHECK_EQ(allocation_.Type(),
           ThreadIsolation::JitAllocationType::kInstructionStream);
  size_t offset = address - address_;
  Tagged<T> tagged(value);
  switch (offset) {
    case InstructionStream::kCodeOffset:
      WriteProtectedPointerHeaderSlot<T, InstructionStream::kCodeOffset>(tagged,
                                                                         tag);
      break;
    case InstructionStream::kRelocationInfoOffset:
      WriteProtectedPointerHeaderSlot<T,
                                      InstructionStream::kRelocationInfoOffset>(
          tagged, tag);
      break;
    default:
      UNREACHABLE();
  }
}

template <typename T>
V8_INLINE void WritableJitAllocation::WriteUnalignedValue(Address address,
                                                          T value) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  DCHECK_GE(address, address_);
  DCHECK_LT(address - address_, size());
  base::WriteUnalignedValue<T>(address, value);
}

template <typename T>
V8_INLINE void WritableJitAllocation::WriteValue(Address address, T value) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  DCHECK_GE(address, address_);
  DCHECK_LT(address - address_, size());
  base::Memory<T>(address) = value;
}

template <typename T>
V8_INLINE void WritableJitAllocation::WriteValue(Address address, T value,
                                                 RelaxedStoreTag) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  DCHECK_GE(address, address_);
  DCHECK_LT(address - address_, size());
  reinterpret_cast<std::atomic<T>*>(address)->store(value,
                                                    std::memory_order_relaxed);
}

void WritableJitAllocation::CopyCode(size_t dst_offset, const uint8_t* src,
                                     size_t num_bytes) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  CopyBytes(reinterpret_cast<uint8_t*>(address_ + dst_offset), src, num_bytes);
}

void WritableJitAllocation::CopyData(size_t dst_offset, const uint8_t* src,
                                     size_t num_bytes) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  CopyBytes(reinterpret_cast<uint8_t*>(address_ + dst_offset), src, num_bytes);
}

void WritableJitAllocation::ClearBytes(size_t offset, size_t len) {
  std::optional<RwxMemoryWriteScope> write_scope =
      WriteScopeForApiEnforcement();
  memset(reinterpret_cast<void*>(address_ + offset), 0, len);
}

WritableJitPage::~WritableJitPage() = default;

WritableJitPage::WritableJitPage(Address addr, size_t size)
    : write_scope_("WritableJitPage"),
      page_ref_(ThreadIsolation::LookupJitPage(addr, size)) {}

WritableJitAllocation WritableJitPage::LookupAllocationContaining(
    Address addr) {
  auto pair = page_ref_.AllocationContaining(addr);
  return WritableJitAllocation(pair.first, pair.second.Size(),
                               pair.second.Type(), false);
}

V8_INLINE WritableFreeSpace WritableJitPage::FreeRange(Address addr,
                                                       size_t size) {
  page_ref_.UnregisterRange(addr, size);
  return WritableFreeSpace(addr, size, true);
}

WritableFreeSpace::~WritableFreeSpace() = default;

// static
V8_INLINE WritableFreeSpace
WritableFreeSpace::ForNonExecutableMemory(base::Address addr, size_t size) {
  return WritableFreeSpace(addr, size, false);
}

V8_INLINE WritableFreeSpace::WritableFreeSpace(base::Address addr, size_t size,
                                               bool executable)
    : address_(addr), size_(static_cast<int>(size)), executable_(executable) {}

template <typename T, size_t offset>
void WritableFreeSpace::WriteHeaderSlot(Tagged<T> value,
                                        RelaxedStoreTag) const {
  Tagged<HeapObject> object = HeapObject::FromAddress(address_);
  // TODO(v8:13355): add validation before the write.
  if constexpr (offset == HeapObject::kMapOffset) {
    TaggedField<T, offset>::Relaxed_Store_Map_Word(object, value);
  } else {
    TaggedField<T, offset>::Relaxed_Store(object, value);
  }
}

#if V8_HAS_PTHREAD_JIT_WRITE_PROTECT

// static
bool RwxMemoryWriteScope::IsSupported() { return true; }

// static
void RwxMemoryWriteScope::SetWritable() { base::SetJitWriteProtected(0); }

// static
void RwxMemoryWriteScope::SetExecutable() { base::SetJitWriteProtected(1); }

#elif V8_HAS_BECORE_JIT_WRITE_PROTECT

// static
bool RwxMemoryWriteScope::IsSupported() {
  return be_memory_inline_jit_restrict_with_witness_supported() != 0;
}

// static
void RwxMemoryWriteScope::SetWritable() {
  be_memory_inline_jit_restrict_rwx_to_rw_with_witness();
}

// static
void RwxMemoryWriteScope::SetExecutable() {
  be_memory_inline_jit_restrict_rwx_to_rx_with_witness();
}

#elif V8_HAS_PKU_JIT_WRITE_PROTECT
// static
bool RwxMemoryWriteScope::IsSupported() {
  static_assert(base::MemoryProtectionKey::kNoMemoryProtectionKey == -1);
  DCHECK(ThreadIsolation::initialized());
  return ThreadIsolation::PkeyIsAvailable();
}

// static
void RwxMemoryWriteScope::SetWritable() {
  DCHECK(ThreadIsolation::initialized());
  if (!IsSupported()) return;

  DCHECK_NE(
      base::MemoryProtectionKey::GetKeyPermission(ThreadIsolation::pkey()),
      base::MemoryProtectionKey::kNoRestrictions);

  base::MemoryProtectionKey::SetPermissionsForKey(
      ThreadIsolation::pkey(), base::MemoryProtectionKey::kNoRestrictions);
}

// static
void RwxMemoryWriteScope::SetExecutable() {
  DCHECK(ThreadIsolation::initialized());
  if (!IsSupported()) return;

  DCHECK_EQ(
      base::MemoryProtectionKey::GetKeyPermission(ThreadIsolation::pkey()),
      base::MemoryProtectionKey::kNoRestrictions);

  base::MemoryProtectionKey::SetPermissionsForKey(
      ThreadIsolation::pkey(), base::MemoryProtectionKey::kDisableWrite);
}

#else  // !V8_HAS_PTHREAD_JIT_WRITE_PROTECT && !V8_TRY_USE_PKU_JIT_WRITE_PROTECT

// static
bool RwxMemoryWriteScope::IsSupported() { return false; }

// static
void RwxMemoryWriteScope::SetWritable() {}

// static
void RwxMemoryWriteScope::SetExecutable() {}

#endif  // V8_HAS_PTHREAD_JIT_WRITE_PROTECT

}  // namespace internal
}  // namespace v8

#endif  // V8_COMMON_CODE_MEMORY_ACCESS_INL_H_
