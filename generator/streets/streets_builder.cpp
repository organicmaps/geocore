#include "generator/streets/streets_builder.hpp"

#include "generator/key_value_storage.hpp"
#include "generator/streets/street_regions_tracing.hpp"
#include "generator/translation.hpp"

#include "coding/internal/file_data.hpp"

#include "indexer/classificator.hpp"
#include "indexer/ftypes_matcher.hpp"

#include "geometry/mercator.hpp"

#include "platform/platform.hpp"

#include "base/logging.hpp"
#include "base/scope_guard.hpp"

#include <utility>

#include "3party/jansson/myjansson.hpp"

using namespace feature;

namespace generator
{
namespace streets
{
StreetsBuilder::StreetsBuilder(RegionFinder const & regionFinder,
                               size_t threadsCount)
  : m_regionFinder{regionFinder}, m_threadsCount{threadsCount}
{
}

void StreetsBuilder::AssembleStreets(std::string const & pathInStreetsTmpMwm)
{
  auto const transform = [this](FeatureBuilder & fb, uint64_t /* currPos */) { AddStreet(fb); };
  if (m_threadsCount == 1)
    ForEachFromDatRawFormat(pathInStreetsTmpMwm, transform);
  else
    ForEachParallelFromDatRawFormat(m_threadsCount, pathInStreetsTmpMwm, transform);
}

void StreetsBuilder::AssembleBindings(std::string const & pathInGeoObjectsTmpMwm)
{
  auto const transform = [this](FeatureBuilder & fb, uint64_t /* currPos */) {
    std::string streetName = fb.GetParams().GetStreet();
    if (!streetName.empty())
    {
      // TODO maybe (lagrunge): add localizations on street:lang tags
      StringUtf8Multilang multilangName;
      multilangName.AddString(StringUtf8Multilang::kDefaultCode, streetName);
      AddStreetBinding(std::move(streetName), fb, multilangName);
    }
  };

  if (m_threadsCount == 1)
    ForEachFromDatRawFormat(pathInGeoObjectsTmpMwm, transform);
  else
    ForEachParallelFromDatRawFormat(m_threadsCount, pathInGeoObjectsTmpMwm, transform);
}

void StreetsBuilder::RegenerateAggregatedStreetsFeatures(
    std::string const & pathStreetsTmpMwm)
{
  auto const aggregatedStreetsTmpFile = GetPlatform().TmpPathForFile();
  SCOPE_GUARD(aggregatedStreetsTmpFileGuard,
              std::bind(Platform::RemoveFileIfExists, aggregatedStreetsTmpFile));
  FeaturesCollector collector(aggregatedStreetsTmpFile);

  std::set<Street const *> processedStreets;
  auto const transform = [&](FeatureBuilder & fb, uint64_t /* currPos */) {
    auto street = m_streetFeatures2Streets.find(fb.GetMostGenericOsmId());
    if (street == m_streetFeatures2Streets.end())
        return;

    if (!processedStreets.insert(street->second).second)
      return;

    WriteAsAggregatedStreet(fb, *street->second, collector);
  };
  ForEachFromDatRawFormat(pathStreetsTmpMwm, transform);

  collector.Finish();

  CHECK(base::RenameFileX(aggregatedStreetsTmpFile, pathStreetsTmpMwm), ());
}

void StreetsBuilder::WriteAsAggregatedStreet(FeatureBuilder & fb, Street const & street,
                                             FeaturesCollector & collector) const
{
  fb.GetParams().name = street.m_name;

  auto const & geometry = street.m_geometry;
  auto const & pin = geometry.GetOrChoosePin();
  fb.SetOsmId(pin.m_osmId);

  if (auto const & pin = geometry.GetPin())
  {
    fb.ResetGeometry();
    fb.SetCenter(pin->m_position);
    collector.Collect(fb);
  }

  auto const * highwayGeometry = geometry.GetHighwayGeometry();
  if (!highwayGeometry)
    return;

  for (auto const & area : highwayGeometry->GetAreaParts())
  {
    fb.ResetGeometry();
    fb.GetParams().SetGeomType(feature::GeomType::Area);
    auto polygon = area.m_border;
    fb.AddPolygon(polygon);
    collector.Collect(fb);
  }

  for (auto const & line : highwayGeometry->GetMultiLine().m_lines)
  {
    for (auto const & segment : line.m_segments)
    {
      fb.ResetGeometry();
      fb.SetLinear();
      for (auto const & point : segment.m_points)
        fb.AddPoint(point);
      collector.Collect(fb);
    }
  }
}

void StreetsBuilder::SaveStreetsKv(RegionGetter const & regionGetter,
                                   std::ostream & streamStreetsKv)
{
  for (auto const & region : m_regions)
  {
    auto const && regionObject = regionGetter(region.first);
    CHECK(regionObject, ());
    SaveRegionStreetsKv(region.second, region.first, *regionObject, streamStreetsKv);
  }
}

void StreetsBuilder::SaveRegionStreetsKv(RegionStreets const & streets, uint64_t regionId,
                                         JsonValue const & regionInfo,
                                         std::ostream & streamStreetsKv)
{
  for (auto const & street : streets)
  {
    auto const & bbox = street.second.m_geometry.GetBbox();
    auto const & pin = street.second.m_geometry.GetOrChoosePin();

    auto const id = KeyValueStorage::SerializeDref(pin.m_osmId.GetEncodedId());
    auto const & value =
        MakeStreetValue(regionId, regionInfo, street.second.m_name, bbox, pin.m_position);
    streamStreetsKv << id << " " << KeyValueStorage::Serialize(value) << "\n";
  }
}

void StreetsBuilder::AddStreet(FeatureBuilder & fb)
{
  if (fb.IsArea())
    return AddStreetArea(fb);

  if (fb.IsPoint())
    return AddStreetPoint(fb);

  CHECK(fb.IsLine(), ());
  AddStreetHighway(fb);
}

void StreetsBuilder::AddStreetHighway(FeatureBuilder & fb)
{
  auto streetRegionInfoGetter = [this](auto const & pathPoint) {
    return this->FindStreetRegionOwner(pathPoint);
  };
  StreetRegionsTracing regionsTracing(fb.GetOuterGeometry(), streetRegionInfoGetter);

  std::lock_guard<std::mutex> lock{m_updateMutex};

  auto && pathSegments = regionsTracing.StealPathSegments();
  for (auto & segment : pathSegments)
  {
    auto && region = segment.m_region;
    auto & street = InsertStreet(region.first, fb.GetName(), fb.GetMultilangName());
    auto const osmId = pathSegments.size() == 1 ? fb.GetMostGenericOsmId() : NextOsmSurrogateId();
    street.m_geometry.AddHighwayLine(osmId, std::move(segment.m_path));

    m_streetFeatures2Streets.emplace(fb.GetMostGenericOsmId(), &street);
  }
}

void StreetsBuilder::AddStreetArea(FeatureBuilder & fb)
{
  auto && region = FindStreetRegionOwner(fb.GetGeometryCenter(), true);
  if (!region)
    return;

  std::lock_guard<std::mutex> lock{m_updateMutex};

  auto & street = InsertStreet(region->first, fb.GetName(), fb.GetMultilangName());
  auto osmId = fb.GetMostGenericOsmId();
  street.m_geometry.AddHighwayArea(osmId, fb.GetOuterGeometry());

  m_streetFeatures2Streets.emplace(osmId, &street);
}

void StreetsBuilder::AddStreetPoint(FeatureBuilder & fb)
{
  auto && region = FindStreetRegionOwner(fb.GetKeyPoint(), true);
  if (!region)
    return;

  std::lock_guard<std::mutex> lock{m_updateMutex};

  auto osmId = fb.GetMostGenericOsmId();
  auto & street = InsertStreet(region->first, fb.GetName(), fb.GetMultilangName());
  street.m_geometry.SetPin({fb.GetKeyPoint(), osmId});

  m_streetFeatures2Streets.emplace(osmId, &street);
}

void StreetsBuilder::AddStreetBinding(std::string && streetName, FeatureBuilder & fb,
                                      StringUtf8Multilang const & multiLangName)
{
  auto const region = FindStreetRegionOwner(fb.GetKeyPoint());
  if (!region)
    return;

  std::lock_guard<std::mutex> lock{m_updateMutex};

  auto & street = InsertStreet(region->first, std::move(streetName), multiLangName);
  street.m_geometry.AddBinding(NextOsmSurrogateId(), fb.GetKeyPoint());
}

boost::optional<KeyValue> StreetsBuilder::FindStreetRegionOwner(m2::PointD const & point,
                                                                bool needLocality)
{
  auto const isStreetAdministrator = [needLocality](KeyValue const & region) {
    auto && address = base::GetJSONObligatoryFieldByPath(*region.second, "properties", "locales",
                                                         "default", "address");

    if (base::GetJSONOptionalField(address, "suburb"))
      return false;
    if (base::GetJSONOptionalField(address, "sublocality"))
      return false;

    if (needLocality && !base::GetJSONOptionalField(address, "locality"))
      return false;

    return true;
  };

  return m_regionFinder(point, isStreetAdministrator);
}

StringUtf8Multilang MergeNames(const StringUtf8Multilang & first,
                               const StringUtf8Multilang & second)
{
  StringUtf8Multilang result;

  auto const fn = [&result](int8_t code, std::string const & name) {
    result.AddString(code, name);
  };
  first.ForEach(fn);
  second.ForEach(fn);
  return result;
}

StreetsBuilder::Street & StreetsBuilder::InsertStreet(uint64_t regionId, std::string && streetName,
                                                      StringUtf8Multilang const & multilangName)
{
  auto & regionStreets = m_regions[regionId];
  StreetsBuilder::Street & street = regionStreets[std::move(streetName)];
  street.m_name = MergeNames(multilangName, street.m_name);
  return street;
}

base::JSONPtr StreetsBuilder::MakeStreetValue(uint64_t regionId, JsonValue const & regionObject,
                                              StringUtf8Multilang const & streetName,
                                              m2::RectD const & bbox, m2::PointD const & pinPoint)
{
  auto streetObject = base::NewJSONObject();

  auto && regionLocales = base::GetJSONObligatoryFieldByPath(regionObject, "properties", "locales");
  auto locales = base::JSONPtr{json_deep_copy(const_cast<json_t *>(regionLocales))};
  auto properties = base::NewJSONObject();
  ToJSONObject(*properties, "locales", std::move(locales));

  Localizator localizator(*properties);
  auto const & localizee = Localizator::EasyObjectWithTranslation(streetName);
  localizator.SetLocale("name", localizee);

  localizator.SetLocale("street", localizee, "address");

  ToJSONObject(*properties, "dref", KeyValueStorage::SerializeDref(regionId));
  ToJSONObject(*streetObject, "properties", std::move(properties));

  auto const & leftBottom = MercatorBounds::ToLatLon(bbox.LeftBottom());
  auto const & rightTop = MercatorBounds::ToLatLon(bbox.RightTop());
  auto const & bboxArray =
      std::vector<double>{leftBottom.m_lon, leftBottom.m_lat, rightTop.m_lon, rightTop.m_lat};
  ToJSONObject(*streetObject, "bbox", std::move(bboxArray));

  auto const & pinLatLon = MercatorBounds::ToLatLon(pinPoint);
  auto const & pinArray = std::vector<double>{pinLatLon.m_lon, pinLatLon.m_lat};
  ToJSONObject(*streetObject, "pin", std::move(pinArray));

  return streetObject;
}

base::GeoObjectId StreetsBuilder::NextOsmSurrogateId()
{
  return base::GeoObjectId{base::GeoObjectId::Type::OsmSurrogate, ++m_osmSurrogateCounter};
}

// static
bool StreetsBuilder::IsStreet(OsmElement const & element)
{
  if (element.GetTagValue("name", {}).empty())
    return false;

  if (element.HasTag("highway") && (element.IsWay() || element.IsRelation()))
    return true;

  if (element.HasTag("place", "square"))
    return true;

  return false;
}

// static
bool StreetsBuilder::IsStreet(FeatureBuilder const & fb)
{
  if (fb.GetName().empty())
    return false;

  auto const & wayChecker = ftypes::IsWayChecker::Instance();
  if (wayChecker(fb.GetTypes()) && (fb.IsLine() || fb.IsArea()))
    return true;

  auto const & squareChecker = ftypes::IsSquareChecker::Instance();
  if (squareChecker(fb.GetTypes()))
    return true;

  return false;
}
}  // namespace streets
}  // namespace generator
