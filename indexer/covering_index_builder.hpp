#pragma once
#include "indexer/cell_id.hpp"
#include "indexer/cell_value_pair.hpp"
#include "indexer/covered_object.hpp"
#include "indexer/feature_covering.hpp"
#include "indexer/interval_index_builder.hpp"
#include "indexer/scales.hpp"

#include "coding/file_container.hpp"
#include "coding/writer.hpp"

#include "base/logging.hpp"
#include "base/macros.hpp"
#include "base/scope_guard.hpp"
#include "base/thread_pool_computational.hpp"

#include "defines.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <boost/sort/sort.hpp>

namespace covering
{
using ObjectsCovering = std::deque<CellValuePair<uint64_t>>;
}  // namespace covering

namespace indexer
{
template <typename BuilderSpec>
class CoveringIndexBuilder
{
public:
  CoveringIndexBuilder(base::thread_pool::computational::ThreadPool & threadPool)
    : m_threadPool{threadPool}
  { }

  void Cover(CoveredObject const & coveredObject, covering::ObjectsCovering & covering) const
  {
    static auto const cellDepth =
        covering::GetCodingDepth<BuilderSpec::kDepthLevels>(scales::GetUpperScale());

    auto const id = coveredObject.GetStoredId();
    auto && cells = m_builderSpec.Cover(coveredObject, cellDepth, m_threadPool);
    for (auto const & cell : cells)
      covering.emplace_back(cell, id);
  }

  bool BuildCoveringIndex(covering::ObjectsCovering && covering,
                          std::string const & localityIndexPath) const
  {
    std::vector<char> buffer;
    buffer.reserve(covering.size() * 10 /* ~ ratio file-size / cell-pair */);
    MemWriter<std::vector<char>> indexWriter{buffer};

    BuildCoveringIndex(std::move(covering), indexWriter, BuilderSpec::kDepthLevels);

    try
    {
      FilesContainerW writer(localityIndexPath, FileWriter::OP_WRITE_TRUNCATE);
      writer.Write(buffer, BuilderSpec::kIndexFileTag);
    }
    catch (Writer::Exception const & e)
    {
      LOG(LERROR, ("Error writing index file:", e.Msg()));
      return false;
    }

    return true;
  }

  template <typename Writer>
  void BuildCoveringIndex(covering::ObjectsCovering && covering, Writer && writer,
                          int depthLevel) const
  {
    // 32 threads block_indirect_sort is fastest for |block_size| (internal parameter) and
    // sizeof(CellValuePair<uint64_t>).
    auto sortThreadsCount = std::min(32u, std::thread::hardware_concurrency());
    boost::sort::block_indirect_sort(covering.begin(), covering.end(), sortThreadsCount);

    BuildIntervalIndex(covering.begin(), covering.end(), std::forward<Writer>(writer),
                       depthLevel * 2 + 1, IntervalIndexVersion::V2);
  }

private:
  BuilderSpec m_builderSpec;
  base::thread_pool::computational::ThreadPool & m_threadPool;
};

struct RegionsIndexBuilderSpec
{
  static constexpr int kDepthLevels = kRegionsDepthLevels;
  static constexpr auto const & kIndexFileTag = REGIONS_INDEX_FILE_TAG;

  std::vector<int64_t> Cover(indexer::CoveredObject const & o, int cellDepth,
                             base::thread_pool::computational::ThreadPool & threadPool) const
  {
    return covering::CoverRegion(o, cellDepth, threadPool);
  }
};

struct GeoObjectsIndexBuilderSpec
{
  static constexpr int kDepthLevels = kGeoObjectsDepthLevels;
  static constexpr auto const & kIndexFileTag = GEO_OBJECTS_INDEX_FILE_TAG;

  std::vector<int64_t> Cover(indexer::CoveredObject const & o, int cellDepth,
                             base::thread_pool::computational::ThreadPool & /* threadPool */) const
  {
    return covering::CoverGeoObject(o, cellDepth);
  }
};

using RegionsIndexBuilder = CoveringIndexBuilder<RegionsIndexBuilderSpec>;
using GeoObjectsIndexBuilder = CoveringIndexBuilder<GeoObjectsIndexBuilderSpec>;
}  // namespace indexer
