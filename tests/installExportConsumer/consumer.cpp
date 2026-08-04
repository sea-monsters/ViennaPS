#include <geometries/psMakePlane.hpp>
#include <psDomain.hpp>

int main() {
  auto domain = viennaps::Domain<double, 2>::New(1.0, 10.0, 10.0);
  viennaps::MakePlane<double, 2>(domain).apply();
  return domain->getLevelSets().empty() ? 1 : 0;
}
