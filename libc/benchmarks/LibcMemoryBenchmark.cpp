//===-- Benchmark memory specific tools -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LibcMemoryBenchmark.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>

namespace llvm {
namespace libc_benchmarks {

// Returns a distribution that samples the buffer to satisfy the required
// alignment.
// When alignment is set, the distribution is scaled down by `Factor` and scaled
// up again by the same amount during sampling.
static std::uniform_int_distribution<uint32_t>
getOffsetDistribution(size_t BufferSize, size_t MaxSizeValue,
                      MaybeAlign AccessAlignment) {
  if (AccessAlignment && *AccessAlignment > AlignedBuffer::Alignment)
    report_fatal_error(
        "AccessAlignment must be less or equal to AlignedBuffer::Alignment");
  if (!AccessAlignment)
    return std::uniform_int_distribution<uint32_t>(0, 0); // Always 0.
  // If we test up to Size bytes, the returned offset must stay under
  // BuffersSize - Size.
  int64_t MaxOffset = BufferSize;
  MaxOffset -= MaxSizeValue;
  MaxOffset -= 1;
  if (MaxOffset < 0)
    report_fatal_error(
        "BufferSize too small to exercise specified Size configuration");
  MaxOffset /= AccessAlignment->value();
  return std::uniform_int_distribution<uint32_t>(0, MaxOffset);
}

class OffsetDistribution {
public:
  OffsetDistribution(size_t BufferSize, size_t MaxSizeValue,
                     MaybeAlign AccessAlignment);

private:
  std::uniform_int_distribution<uint32_t> Distribution;
  size_t Factor;
};

OffsetDistribution::OffsetDistribution(size_t BufferSize, size_t MaxSizeValue,
                                       MaybeAlign AccessAlignment)
    : Distribution(
          getOffsetDistribution(BufferSize, MaxSizeValue, AccessAlignment)),
      Factor(AccessAlignment.valueOrOne().value()) {}

// Precomputes offset where to insert mismatches between the two buffers.
class MismatchOffsetDistribution {
public:
  MismatchOffsetDistribution(size_t BufferSize, size_t MaxSizeValue, size_t MismatchAt);

private:
  size_t MismatchAt;
  std::vector<size_t> MismatchIndices;
  std::uniform_int_distribution<size_t> MismatchIndexSelector;
};

MismatchOffsetDistribution::MismatchOffsetDistribution(size_t BufferSize,
                                                       size_t MaxSizeValue,
                                                       size_t MismatchAt)
    : MismatchAt(MismatchAt) {
  if (MismatchAt <= 1)
    return;
  for (size_t I = MaxSizeValue + 1; I < BufferSize; I += MaxSizeValue)
    MismatchIndices.push_back(I);
  if (MismatchIndices.empty())
    report_fatal_error("Unable to generate mismatch");
  MismatchIndexSelector =
      std::uniform_int_distribution<size_t>(0, MismatchIndices.size() - 1);
}

static size_t getL1DataCacheSize() {
  const std::vector<CacheInfo> &CacheInfos = HostState::get().Caches;
  const auto IsL1DataCache = [](const CacheInfo &CI) {
    return CI.Type == "Data" && CI.Level == 1;
  };
  const auto CacheIt = find_if(CacheInfos, IsL1DataCache);
  if (CacheIt != CacheInfos.end())
    return CacheIt->Size;
  report_fatal_error("Unable to read L1 Cache Data Size");
}

static constexpr int64_t KiB = 1024;
static constexpr int64_t ParameterStorageBytes = 4 * KiB;
static constexpr int64_t L1LeftAsideBytes = 1 * KiB;

static size_t getAvailableBufferSize() {
  return getL1DataCacheSize() - L1LeftAsideBytes - ParameterStorageBytes;
}

class ParameterBatch {
public:
  ParameterBatch(size_t BufferCount);
  size_t getBatchBytes() const;
  void checkValid(const ParameterType &P) const;

private:
  size_t BufferSize;
  size_t BatchSize;
  std::vector<ParameterType> Parameters;
};

ParameterBatch::ParameterBatch(size_t BufferCount)
    : BufferSize(getAvailableBufferSize() / BufferCount),
      BatchSize(ParameterStorageBytes / sizeof(ParameterType)),
      Parameters(BatchSize) {
  if (BufferSize <= 0 || BatchSize < 100)
    report_fatal_error("Not enough L1 cache");
  const size_t ParameterBytes = Parameters.size() * sizeof(ParameterType);
  const size_t BufferBytes = BufferSize * BufferCount;
  if (ParameterBytes + BufferBytes + L1LeftAsideBytes > getL1DataCacheSize())
    report_fatal_error(
        "We're splitting a buffer of the size of the L1 cache between a data "
        "buffer and a benchmark parameters buffer, so by construction the "
        "total should not exceed the size of the L1 cache");
}

size_t ParameterBatch::getBatchBytes() const {
  size_t BatchBytes = 0;
  for (auto &P : Parameters)
    BatchBytes += P.SizeBytes;
  return BatchBytes;
}

void ParameterBatch::checkValid(const ParameterType &P) const {
  if (P.OffsetBytes + P.SizeBytes >= BufferSize)
    report_fatal_error(
        llvm::Twine("Call would result in buffer overflow: Offset=")
            .concat(llvm::Twine(P.OffsetBytes))
            .concat(", Size=")
            .concat(llvm::Twine(P.SizeBytes))
            .concat(", BufferSize=")
            .concat(llvm::Twine(BufferSize)));
}

class CopySetup {
public:
  CopySetup();

private:
  ParameterBatch ParameterBatch;
  AlignedBuffer SrcBuffer;
  AlignedBuffer DstBuffer;
};

CopySetup::CopySetup()
    : ParameterBatch(2), SrcBuffer(ParameterBatch::BufferSize),
      DstBuffer(ParameterBatch::BufferSize) {}

class MoveSetup {
public:
  MoveSetup();

private:
  ParameterBatch ParameterBatch;
  AlignedBuffer Buffer;
};

MoveSetup::MoveSetup()
    : ParameterBatch(3), Buffer(ParameterBatch::BufferSize * 3) {}

class ComparisonSetup {
public:
  ComparisonSetup();

private:
  ParameterBatch ParameterBatch;
  AlignedBuffer LhsBuffer;
  AlignedBuffer RhsBuffer;
};

ComparisonSetup::ComparisonSetup()
    : ParameterBatch(2), LhsBuffer(ParameterBatch::BufferSize),
      RhsBuffer(ParameterBatch::BufferSize) {
  // The memcmp buffers always compare equal.
  memset(LhsBuffer.begin(), 0xF, BufferSize);
  memset(RhsBuffer.begin(), 0xF, BufferSize);
}

class SetSetup {
public:
  SetSetup();

private:
  ParameterBatch ParameterBatch;
  AlignedBuffer DstBuffer;
};

SetSetup::SetSetup()
    : ParameterBatch(1), DstBuffer(ParameterBatch::BufferSize) {}

} // namespace libc_benchmarks
} // namespace llvm
