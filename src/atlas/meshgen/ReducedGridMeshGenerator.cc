/*
 * (C) Copyright 1996-2014 ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */



#include <numeric>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "eckit/runtime/Context.h"
#include "eckit/config/Configurable.h"
#include "eckit/config/Resource.h"
#include "eckit/filesystem/LocalPathName.h"
#include "eckit/geometry/Point3.h"

#include "atlas/atlas_config.h"
#include "atlas/util/Array.h"
#include "atlas/util/ArrayView.h"
#include "atlas/util/IndexView.h"
#include "atlas/Field.h"
#include "atlas/FunctionSpace.h"
#include "atlas/Mesh.h"
#include "atlas/Parameters.h"
#include "atlas/meshgen/EqualRegionsPartitioner.h"
#include "atlas/grids/ReducedGrid.h"
#include "atlas/meshgen/ReducedGridMeshGenerator.h"
#include "atlas/GridDistribution.h"
#include "atlas/util/Bitflags.h"
#include "atlas/io/Gmsh.h"

#ifdef ATLAS_HAVE_TRANS
#include "atlas/trans/TransPartitioner.h"
#endif

#define DEBUG_OUTPUT 0

using namespace eckit;
using namespace atlas::grids;

using atlas::util::Topology;

namespace atlas {
namespace meshgen {

namespace {
static double to_rad = M_PI/180.;
static double to_deg = 180.*M_1_PI;
}

struct Region
{
  int north;
  int south;
  Array<int> elems;
  int ntriags;
  int nquads;
  int nnodes;
  std::vector<int> lat_begin;
  std::vector<int> lat_end;
  std::vector<int> nb_lat_elems;
};

ReducedGridMeshGenerator::ReducedGridMeshGenerator()
{
  configure_defaults();
}


ReducedGridMeshGenerator::ReducedGridMeshGenerator(const eckit::Parametrisation& p)
{
  configure_defaults();

  bool include_pole;
  if( p.get("include_pole",include_pole) )
    options.set("include_pole",include_pole);

  bool patch_pole;
  if( p.get("patch_pole",patch_pole) )
    options.set("patch_pole",patch_pole);

  bool unique_pole;
  if( p.get("unique_pole",unique_pole) )
    options.set("unique_pole",unique_pole);

  bool three_dimensional;
  if( p.get("three_dimensional",three_dimensional) )
    options.set("three_dimensional",three_dimensional);

  size_t nb_parts;
  if( p.get("nb_parts",nb_parts) )
    options.set("nb_parts",nb_parts);

  size_t part;
  if( p.get("part",part) )
    options.set("part",part);

  double angle;
  if( p.get("angle",angle) )
    options.set("angle",angle);

  bool triangulate;
  if( p.get("triangulate",triangulate) )
    options.set("triangulate",triangulate);
}


void ReducedGridMeshGenerator::configure_defaults()
{
  // This option creates a point at the pole when true
  options.set("include_pole",bool( Resource<bool>("--include_pole;atlas.meshgen.include_pole", false ) ) );

  // This option sets the part that will be generated
  options.set("patch_pole", bool(Resource<bool>("--patch_pole;atlas.meshgen.patch_pole",false ) ) );

  // This option disregards multiple poles in grid (e.g. lonlat up to poles) and connects elements
  // to the first node only. Note this option will only be looked at in case other option
  // "three_dimensional"==true
  options.set("unique_pole", bool(Resource<bool>("--unique_pole;atlas.meshgen.unique_pole",true ) ) );

  // This option creates elements that connect east to west at greenwich meridian
  // when true, instead of creating periodic ghost-points at east boundary when false
  options.set("three_dimensional",bool(Resource<bool>("--three_dimensional;atlas.meshgen.three_dimensional",false ) ));

  // This option sets number of parts the mesh will be split in
  options.set("nb_parts",eckit::mpi::size());

  // This option sets the part that will be generated
  options.set("part",eckit::mpi::rank());

  // Experimental option. The result is a non-standard Reduced Gaussian Grid, with a ragged Greenwich line
  options.set("stagger", bool( Resource<bool>("--stagger;meshgen.stagger", false ) ) );

  // This option sets the maximum angle deviation for a quadrilateral element
  // angle = 30  -->  minimises number of triangles
  // angle = 0   -->  maximises number of triangles
  options.set<double>("angle", Resource< double > ( "atlas.meshgen.angle", 0. ) );

  options.set<bool>("triangulate", Resource< bool > ( "--triangulate;atlas.meshgen.triangulate", 1.) );
}

void ReducedGridMeshGenerator::generate(const Grid& grid, Mesh& mesh ) const
{
//  const_cast<ReducedGridMeshGenerator*>(this)->options.set<bool>("patch_pole",false);
//  const_cast<ReducedGridMeshGenerator*>(this)->options.set<bool>("include_pole",false);
//  const_cast<ReducedGridMeshGenerator*>(this)->options.set<bool>("unique_pole",false);
//  const_cast<ReducedGridMeshGenerator*>(this)->options.set<bool>("triangulate",false);
//  const_cast<ReducedGridMeshGenerator*>(this)->options.set<bool>("three_dimensional",true);
//  DEBUG_VAR(grid.grid_type_str());
//  DEBUG_VAR(grid.gridType());
//  DEBUG_VAR(grid.className());
//  DEBUG_VAR(grid.N());
//  DEBUG_VAR(grid.nlat());
//  DEBUG_VAR(grid.nlon(0));
//  DEBUG_VAR(grid.lat(0));
//  DEBUG_VAR(grid.lon(0,0));
//  DEBUG_VAR(grid.lon(0,1));
//  DEBUG_VAR(grid.nlon(1));
//  DEBUG_VAR(grid.nlon(grid.nlat()/2));
//  DEBUG_VAR(grid.nlon(grid.nlat()/2-1));

  int nb_parts = options.get<size_t>("nb_parts");

#ifdef ATLAS_HAVE_TRANS
  const grids::ReducedGrid* rg = dynamic_cast<const grids::ReducedGrid*>(&grid);
  if( !rg )
    throw eckit::BadCast("Grid could not be cast to a ReducedGrid",Here());


  std::string partitioner = Resource<std::string>("atlas.meshgen.partitioner",std::string("trans"));
  if( rg->nlat()%2 == 1 ) // odd number of latitudes
    partitioner = "eqreg";

  if( nb_parts == 1 ) // No partitioner required
    partitioner = "eqreg";

  if( partitioner == "trans" )
  {
    if( nb_parts != eckit::mpi::size() )
    {
      std::stringstream msg; msg << "The default TransPartitioner is not equiped to handle nb_parts != mpi::size():\n"
                                 << "( " << nb_parts << " != " << eckit::mpi::size() << " )\n"
                                 << "Please configure Resource: atlas.meshgen.partitioner=eqreg";
      throw UserError(msg.str(),Here());
    }
    GridDistribution::Ptr distribution( trans::TransPartitioner(*rg).distribution() );
    generate( grid, *distribution, mesh );
  }
  else
  {
    GridDistribution::Ptr distribution( EqualRegionsPartitioner(grid,nb_parts).distribution() );
    generate( grid, *distribution, mesh );
  }
#else
  GridDistribution::Ptr distribution( EqualRegionsPartitioner(grid,nb_parts).distribution() );
  generate( grid, *distribution, mesh );
#endif
}

void ReducedGridMeshGenerator::generate(const Grid& grid, const GridDistribution& distribution, Mesh& mesh ) const
{
  const grids::ReducedGrid* rg = dynamic_cast<const grids::ReducedGrid*>(&grid);
  if( !rg )
    throw eckit::BadCast("Grid could not be cast to a ReducedGrid",Here());

  int mypart   = options.get<size_t>("part");

  Region region;
  generate_region(*rg,distribution,mypart,region);

  generate_mesh(*rg,distribution,region,mesh);
  mesh.set_grid(*rg);
}


void ReducedGridMeshGenerator::generate_region(const ReducedGrid& rg,
                                               const std::vector<int>& parts,
                                               int mypart,
                                               Region& region) const
{
  double max_angle          = options.get<double>("angle");
  bool   triangulate_quads  = options.get<bool>("triangulate");
  bool   three_dimensional  = options.get<bool>("three_dimensional");
  bool   has_north_pole = rg.lat(0) == 90;
  bool   has_south_pole = rg.lat(rg.nlat()-1) == -90;
  bool   unique_pole        = options.get<bool>("unique_pole") && three_dimensional && has_north_pole && has_south_pole;

  int n;
  /*
  Find min and max latitudes used by this part.
  */
  n=0;
  int lat_north=-1;
  for( int jlat=0; jlat<rg.nlat(); ++jlat) {
    for( int jlon=0; jlon<rg.nlon(jlat); ++jlon) {
      if( parts[n] == mypart ) {
        lat_north=jlat;
        goto end_north;
      }
      ++n;
    }
  } end_north:

  n=rg.npts()-1;
  int lat_south=-1;
  for( int jlat=rg.nlat()-1; jlat>=0; --jlat) {
    //Log::info() << jlat << std::setw(5) << rg.nlon(jlat) << std::endl;
    for( int jlon=rg.nlon(jlat)-1; jlon>=0; --jlon) {
      if( parts[n] == mypart ) {
        lat_south=jlat;
        goto end_south;
      }
      --n;
    }
  } end_south:


  std::vector<int> offset(rg.nlat(),0);

  n=0;
  for( int jlat=0; jlat<rg.nlat(); ++jlat )
  {
    offset[jlat]=n;
    n+=rg.nlon(jlat);
  };

  /*
  We need to connect to next region
  */
  if( lat_north-1 >=0             && rg.nlon(lat_north-1) > 0 )
    --lat_north;
  if( lat_south+1 <= rg.nlat()-1 && rg.nlon(lat_south+1) > 0 )
    ++lat_south;
  region.lat_begin.resize(rg.nlat(),-1);
  region.lat_end.resize(rg.nlat(),-1);
  region.nb_lat_elems.resize(rg.nlat(),0);
  region.north = lat_north;
  region.south = lat_south;

  ArrayShape shape = make_shape(region.south-region.north, 4*rg.nlonmax(), 4);
  // eckit::Log::info(Here())  << "allocating elems" <<  "(" << extents[0] << "," << extents[1] << "," << extents[2] << ")" << std::endl;
  region.elems.resize(shape);
  region.elems = -1;

  int nelems=0;
  region.nquads=0;
  region.ntriags=0;

  ArrayView<int,3> elemview(region.elems);

  bool stagger = options.get<bool>("stagger");
  for (int jlat=lat_north; jlat<lat_south; ++jlat)
  {
//    std::stringstream filename; filename << "/tmp/debug/"<<jlat;
//    std::ofstream debug_file;
//    debug_file.open(filename.str().c_str(), std::ios_base::out);
    int ilat, latN, latS;
    int ipN1, ipN2, ipS1, ipS2;
    double xN1, xN2, yN, xS1, xS2, yS;
    double dN1S2, dS1N2, dN2S2;
    bool try_make_triangle_up, try_make_triangle_down, try_make_quad;
    bool add_triag, add_quad;

    ilat = jlat-region.north;

    ArrayView<int,2> lat_elems_view = elemview[ilat];

    latN = jlat;
    latS = jlat+1;
    yN = rg.lat(latN);
    yS = rg.lat(latS);

    int beginN, beginS, endN, endS;

    beginN = 0;
    endN   = rg.nlon(latN); // include periodic point
    if( yN == 90 && unique_pole )
      endN = beginN;


    beginS = 0;
    endS   = rg.nlon(latS); // include periodic point
    if( yS == -90 && unique_pole )
      endS = beginS;

    ipN1 = beginN;
    ipS1 = beginS;
    ipN2 = std::min(ipN1+1,endN);
    ipS2 = std::min(ipS1+1,endS);

    int jelem=0;

#if DEBUG_OUTPUT
    eckit::Log::info(Here())  << "=================\n";
    //debug_file  << "=================\n";
#endif

    while (true)
    {
      if( ipN1 == endN && ipS1 == endS ) break;

      //ASSERT(offset[latN]+ipN1 < parts.size());
      //ASSERT(offset[latS]+ipS1 < parts.size());

      int pN1, pS1, pN2, pS2;
      if( ipN1 != rg.nlon(latN) )
        pN1 = parts[offset[latN]+ipN1];
      else
        pN1 = parts[offset[latN]+ipN1-1];
      if( ipS1 != rg.nlon(latS) )
        pS1 = parts[offset[latS]+ipS1];
      else
        pS1 = parts[offset[latS]+ipS1-1];

      if( ipN2 == rg.nlon(latN) )
        pN2 = pN1;
      else
        pN2 = parts[offset[latN]+ipN2];
      if( ipS2 == rg.nlon(latS) )
        pS2 = pS1;
      else
        pS2 = parts[offset[latS]+ipS2];

      //eckit::Log::info(Here())  << ipN1 << "("<<pN1<<") " << ipN2 <<"("<<pN2<<")" <<  std::endl;
      //eckit::Log::info(Here())  << ipS1 << "("<<pS2<<") " << ipS2 <<"("<<pS2<<")" <<  std::endl;


      xN1 = rg.lon(latN,ipN1) * to_rad;
      xN2 = rg.lon(latN,ipN2) * to_rad;
      xS1 = rg.lon(latS,ipS1) * to_rad;
      xS2 = rg.lon(latS,ipS2) * to_rad;

      if( stagger && (latN+1)%2==0 ) xN1 += M_PI/static_cast<double>(rg.nlon(latN));
      if( stagger && (latN+1)%2==0 ) xN2 += M_PI/static_cast<double>(rg.nlon(latN));
      if( stagger && (latS+1)%2==0 ) xS1 += M_PI/static_cast<double>(rg.nlon(latS));
      if( stagger && (latS+1)%2==0 ) xS2 += M_PI/static_cast<double>(rg.nlon(latS));

#if DEBUG_OUTPUT
      eckit::Log::info(Here())  << "-------\n";
      //debug_file << "-------\n";
#endif
      // eckit::Log::info(Here())  << "  access  " << region.elems.stride(0)*(jlat-region.north) + region.elems.stride(1)*jelem + 5 << std::endl;
//      eckit::Log::info(Here())  << ipN1 << "("<< xN1 << ")  " << ipN2 <<  "("<< xN2 << ")  " << std::endl;
//      eckit::Log::info(Here())  << ipS1 << "("<< xS1 << ")  " << ipS2 <<  "("<< xS2 << ")  " << std::endl;
      try_make_triangle_up   = false;
      try_make_triangle_down = false;
      try_make_quad = false;


// ------------------------------------------------
// START RULES
// ------------------------------------------------

      const double dxN = std::abs(xN2-xN1);
      const double dxS = std::abs(xS2-xS1);
      const double dx  = std::min(dxN,dxS);
      const double alpha1 = ( dx==0. ? 0. : std::atan2((xN1-xS1)/dx,1.) * to_deg );
      const double alpha2 = ( dx==0. ? 0. : std::atan2((xN2-xS2)/dx,1.) * to_deg );
      if( std::abs(alpha1) <= max_angle && std::abs(alpha2) <= max_angle )
      {
        if( triangulate_quads )
        {
          if( false ) //std::abs(alpha1) < 1 && std::abs(alpha2) < 1)
          {
            try_make_triangle_up   = (jlat+ipN1) % 2;
            try_make_triangle_down = (jlat+ipN1+1) % 2;
          }
          else
          {
            dN1S2 = std::abs(xN1-xS2);
            dS1N2 = std::abs(xS1-xN2);
            dN2S2 = std::abs(xN2-xS2);
            // eckit::Log::info(Here())  << "  dN1S2 " << dN1S2 << "   dS1N2 " << dS1N2 << "   dN2S2 " << dN2S2 << std::endl;
            if (dN1S2 == dS1N2)
            {
              try_make_triangle_up   = (jlat+ipN1) % 2;
              try_make_triangle_down = (jlat+ipN1+1) % 2;
            }
            else if (dN1S2 < dS1N2)
            {
              if (ipS1 != ipS2) { try_make_triangle_up = true; }
              else { try_make_triangle_down = true ; }
            }
            else if (dN1S2 > dS1N2)
            {
              if (ipN1 != ipN2) { try_make_triangle_down = true;}
              else { try_make_triangle_up = true; }
            }
            else
            {
              throw eckit::Exception("Should not be here", Here());
            }
          }

        }
        else
        {
          if     ( ipN1 == ipN2 ) try_make_triangle_up   = true;
          else if( ipS1 == ipS2 ) try_make_triangle_down = true;
          else                    try_make_quad          = true;

//          try_make_quad          = true;
        }
      }
      else
      {
        dN1S2 = std::abs(xN1-xS2);
        dS1N2 = std::abs(xS1-xN2);
        dN2S2 = std::abs(xN2-xS2);
        // eckit::Log::info(Here())  << "  dN1S2 " << dN1S2 << "   dS1N2 " << dS1N2 << "   dN2S2 " << dN2S2 << std::endl;
        if( (dN1S2 <= dS1N2) && (ipS1 != ipS2) ) { try_make_triangle_up = true;}
        else if( (dN1S2 >= dS1N2) && (ipN1 != ipN2) ) { try_make_triangle_down = true;}
        else Exception("Should not try to make a quadrilateral!",Here());
      }
// ------------------------------------------------
// END RULES
// ------------------------------------------------


#if DEBUG_OUTPUT
      DEBUG_VAR(jelem);
      //debug_file << "jelem = " << jelem << '\n';
#endif

      ArrayView<int,1> elem = lat_elems_view[jelem];

      if( try_make_quad )
      {
        // add quadrilateral
#if DEBUG_OUTPUT
        eckit::Log::info(Here())  << "          " << ipN1 << "  " << ipN2 << '\n';
        eckit::Log::info(Here())  << "          " << ipS1 << "  " << ipS2 << '\n';
        //debug_file << "          " << ipN1 << "  " << ipN2 << '\n';
        //debug_file << "          " << ipS1 << "  " << ipS2 << '\n';
#endif
        elem[0] = ipN1;
        elem[1] = ipS1;
        elem[2] = ipS2;
        elem[3] = ipN2;
        add_quad = false;
        int np[] = {pN1, pN2, pS1, pS2};
        int cnt_mypart = std::count(np, np+4, mypart);
        if( cnt_mypart > 0 )
        {
          int pcnts[4];
          for( int j=0; j<4; ++j )
            pcnts[j] = std::count(np, np+4, np[j]);
          int cnt_max = *std::max_element(pcnts,pcnts+4);

          if( latN == 0 )
          {
            if( pN1 == mypart )
            {
              add_quad = true;
            }
          }
          else if ( latS == rg.nlat()-1 )
          {
            if( pS2 == mypart )
            {
              add_quad = true;
            }
          }
          else if( cnt_mypart > 2 ) // 3 or 4 points belong to mypart
          {
            add_quad = true;
          }
          else if( cnt_max < 3 ) // 3 or 4 points don't belong to mypart
          {
            if( 0.5*(yN+yS) > 1e-6)
            {
              if ( pS1 == mypart )  add_quad = true;
            }
            else
            {
              if ( pN2 == mypart )  add_quad = true;
            }
          }
        }
        if (add_quad)
        {
          ++region.nquads;
          ++jelem;
          ++nelems;

          if( region.lat_begin[latN] == -1 ) region.lat_begin[latN] = ipN1;
          if( region.lat_begin[latS] == -1 ) region.lat_begin[latS] = ipS1;
          region.lat_begin[latN] = std::min(region.lat_begin[latN], ipN1);
          region.lat_begin[latS] = std::min(region.lat_begin[latS], ipS1);
          region.lat_end[latN] = std::max(region.lat_end[latN], ipN2);
          region.lat_end[latS] = std::max(region.lat_end[latS], ipS2);
        }
        ipN1=ipN2;
        ipS1=ipS2;
      }
      else if( try_make_triangle_down  ) // make triangle down
      {
        // triangle without ip3
#if DEBUG_OUTPUT
        eckit::Log::info(Here())  << "          " << ipN1 << "  " << ipN2 << '\n';
        eckit::Log::info(Here())  << "          " << ipS1 << '\n';
        //debug_file  << "          " << ipN1 << "  " << ipN2 << '\n';
        //debug_file  << "          " << ipS1 << '\n';
#endif
        elem[0] = ipN1;
        elem[1] = ipS1;
        elem[2] = -1;
        elem[3] = ipN2;

        add_triag = false;

        int cnt_mypart = 0;
        int np[3] = {pN1, pN2, pS1};
        for( int j=0; j<3; ++j )
          if (np[j]==mypart) ++cnt_mypart;

        if( latN == 0 )
        {
          if( pN1 == mypart )
          {
            add_triag = true;
          }
        }
        else if ( latS == rg.nlat()-1 )
        {
          if( pS1 == mypart )
          {
            add_triag = true;
          }
        }
        else if( cnt_mypart > 1 )
        {
          add_triag=true;
        }
        else if( cnt_mypart == 1)
        {
          int pcnts[3];
          for( int j=0; j<3; ++j )
            pcnts[j] = std::count(np, np+3, np[j]);
          int cnt_max = *std::max_element(pcnts,pcnts+3);
          if( cnt_max == 1)
          {
            if( 0.5*(yN+yS) > 1e-6 )
            {
              if( pS1 == mypart )
                add_triag = true;
            }
            else
            {
              if( pN1 == mypart )
                add_triag = true;
            }
          }
        }
        if (add_triag)
        {
          ++region.ntriags;
          ++jelem;
          ++nelems;

          if( region.lat_begin[latN] == -1 ) region.lat_begin[latN] = ipN1;
          if( region.lat_begin[latS] == -1 ) region.lat_begin[latS] = ipS1;
          region.lat_begin[latN] = std::min(region.lat_begin[latN], ipN1);
          region.lat_begin[latS] = std::min(region.lat_begin[latS], ipS1);
          region.lat_end[latN] = std::max(region.lat_end[latN], ipN2);
          region.lat_end[latS] = std::max(region.lat_end[latS], ipS1);
        }
        ipN1=ipN2;
        // and ipS1=ipS1;

      }
      else // make triangle up
      {
        // triangle without ip4
#if DEBUG_OUTPUT
        eckit::Log::info(Here())  << "          " << ipN1 << '\n';
        eckit::Log::info(Here())  << "          " << ipS1 << "  " << ipS2 << '\n';
        //debug_file  << "          " << ipN1 << '\n';
        //debug_file  << "          " << ipS1 << "  " << ipS2 << '\n';
#endif
        elem[0] = ipN1;
        elem[1] = ipS1;
        elem[2] = ipS2;
        elem[3] = -1;

        add_triag = false;


        int cnt_mypart = 0;
        int np[3] = {pN1, pS1, pS2};
        for( int j=0; j<3; ++j )
          if (np[j]==mypart) ++cnt_mypart;

        if( latN == 0 )
        {
          if( pN1 == mypart )
          {
            add_triag = true;
          }
        }
        else if ( latS == rg.nlat()-1 )
        {
          if( pS2 == mypart )
          {
            add_triag = true;
          }
        }
        else if( cnt_mypart > 1 )
        {
          add_triag=true;
        }
        else if( cnt_mypart == 1)
        {
          int pcnts[3];
          for( int j=0; j<3; ++j )
            pcnts[j] = std::count(np, np+3, np[j]);
          int cnt_max = *std::max_element(pcnts,pcnts+3);
          if( cnt_max == 1)
          {
            if( latN == 0 || latS == rg.nlat()-1 )
            {
              add_triag = true;
            }
            else if( 0.5*(yN+yS) > 1e-6 )
            {
              if( pS2 == mypart )
                add_triag = true;
            }
            else
            {
              if( pN1 == mypart )
                add_triag = true;
            }
          }
        }

        if (add_triag)
        {
          ++region.ntriags;
          ++jelem;
          ++nelems;

          if( region.lat_begin[latN] == -1 ) region.lat_begin[latN] = ipN1;
          if( region.lat_begin[latS] == -1 ) region.lat_begin[latS] = ipS1;
          region.lat_begin[latN] = std::min(region.lat_begin[latN], ipN1);
          region.lat_begin[latS] = std::min(region.lat_begin[latS], ipS1);
          region.lat_end[latN] = std::max(region.lat_end[latN], ipN1);
          region.lat_end[latS] = std::max(region.lat_end[latS], ipS2);
        }
        ipS1=ipS2;
        // and ipN1=ipN1;

      }
      ipN2 = std::min(endN,ipN1+1);
      ipS2 = std::min(endS,ipS1+1);
    }
    region.nb_lat_elems[jlat] = jelem;
    region.lat_end[latN] = std::min(region.lat_end[latN], rg.nlon(latN)-1);
    region.lat_end[latS] = std::min(region.lat_end[latS], rg.nlon(latS)-1);
    if( yN == 90 && unique_pole )
      region.lat_end[latN] = rg.nlon(latN)-1;
    if( yS == -90 && unique_pole )
      region.lat_end[latS] = rg.nlon(latS)-1;

    //debug_file.close();

  } // for jlat

//  eckit::Log::info(Here())  << "nb_triags = " << region.ntriags << std::endl;
//  eckit::Log::info(Here())  << "nb_quads = " << region.nquads << std::endl;
//  eckit::Log::info(Here())  << "nb_elems = " << nelems << std::endl;

  int nb_region_nodes = 0;

  for( int jlat=lat_north; jlat<=lat_south; ++jlat ) {
    region.lat_begin[jlat] = std::max( 0, region.lat_begin[jlat] );
    nb_region_nodes += region.lat_end[jlat]-region.lat_begin[jlat]+1;

    // Count extra periodic node to be added in this case
    if( region.lat_end[jlat] == rg.nlon(jlat)-1 ) ++nb_region_nodes;

  }

  region.nnodes = nb_region_nodes;
  if (region.nnodes == 0)
  {
    throw Exception("Trying to generate mesh with too many partitions. Reduce the number of partitions.",Here());
  }
#if DEBUG_OUTPUT
  DEBUG("End of generate_region()");
#endif
}

void ReducedGridMeshGenerator::generate_mesh(const ReducedGrid& rg,
                                             const std::vector<int>& parts,
                                             const Region& region,
                                             Mesh& mesh ) const
{
  double tol = 1e-3;

  int mypart = options.get<size_t>("part");
  int nparts = options.get<size_t>("nb_parts");
  int n, l;

  bool has_north_pole = rg.lat(0) == 90 && rg.nlon(0) > 0;
  bool has_south_pole = rg.lat(rg.nlat()-1) == -90 && rg.nlon(rg.nlat()-1) > 0;

  bool include_north_pole = (mypart == 0       ) && options.get<bool>("include_pole") && !has_north_pole;
  bool include_south_pole = (mypart == nparts-1) && options.get<bool>("include_pole") && !has_south_pole;
  bool three_dimensional  = options.get<bool>("three_dimensional");
  bool patch_north_pole   = (mypart == 0       ) && options.get<bool>("patch_pole") && three_dimensional
                            && !has_north_pole && rg.nlon(1) > 0;
  bool patch_south_pole   = (mypart == nparts-1) && options.get<bool>("patch_pole") && three_dimensional
                            && !has_south_pole && rg.nlon(rg.nlat()-2) > 0;

  if( three_dimensional && nparts != 1 )
    throw BadParameter("Cannot generate three_dimensional mesh in parallel",Here());
  int nnodes  = region.nnodes;
  int ntriags = region.ntriags;
  int nquads  = region.nquads;


  if (include_north_pole) {
    ++nnodes;
    ntriags += rg.nlon(0);
  }
  else if (patch_north_pole) {
    ntriags += rg.nlon(0)-2;
  }
  if (include_south_pole) {
    ++nnodes;
    ntriags += rg.nlon(rg.nlat()-1);
  }
  else if (patch_south_pole) {
    ntriags += rg.nlon(rg.nlat()-1)-2;
  }
  if (three_dimensional) {
    for( int jlat=0; jlat<rg.nlat(); ++jlat )
    {
      if( rg.nlon(jlat) > 0 )
        --nnodes;
    }
  }

#if DEBUG_OUTPUT
  DEBUG_VAR(include_north_pole);
  DEBUG_VAR(include_south_pole);
  DEBUG_VAR(three_dimensional);
  DEBUG_VAR(patch_north_pole);
  DEBUG_VAR(patch_south_pole);
  DEBUG_VAR(rg.npts());
  DEBUG_VAR(nnodes);
  DEBUG_VAR(ntriags);
  DEBUG_VAR(nquads);
#endif


  std::vector<int> offset_glb(rg.nlat());
  std::vector<int> offset_loc(region.south-region.north+1,0);

  n=0;
  for( int jlat=0; jlat<rg.nlat(); ++jlat )
  {
    offset_glb[jlat]=n;
    n+=rg.nlon(jlat);
  };

  std::vector<int> periodic_glb(rg.nlat());

  if( !three_dimensional )
  {
    for( int jlat=0; jlat<rg.nlat(); ++jlat )
    {
      if( rg.nlon(jlat) > 0 )
      {
        periodic_glb[jlat] = n;
        ++n;
      }
    }
  }
  else
  {
    for( int jlat=0; jlat<rg.nlat(); ++jlat )
    {
      if( rg.nlon(jlat) > 0 )
      {
        periodic_glb[jlat] = offset_glb[jlat] + rg.nlon(jlat)-1;
      }
    }
  }


  ArrayShape shape = make_shape(nnodes,Field::UNDEF_VARS);

  if( !mesh.has_function_space("nodes") )
  mesh.create_function_space( "nodes","LagrangeP1",shape );

  FunctionSpace& nodes = mesh.function_space( "nodes" );

  nodes.metadata().set<long>("type",static_cast<int>(Entity::NODES));

  ArrayView<double,2> lonlat        ( nodes.create_field<double>("lonlat",   2, IF_EXISTS_RETURN) );
  ArrayView<gidx_t,1> glb_idx       ( nodes.create_field<gidx_t>("glb_idx",       1, IF_EXISTS_RETURN) );
  ArrayView<int,   1> part          ( nodes.create_field<int   >("partition",     1, IF_EXISTS_RETURN) );
  ArrayView<int,   1> flags         ( nodes.create_field<int   >("flags",         1, IF_EXISTS_RETURN) );

  bool stagger = options.get<bool>("stagger");

  int jnode=0;
  l=0;
  for( int jlat=region.north; jlat<=region.south; ++jlat )
  {
    int ilat=jlat-region.north;
    offset_loc[ilat]=l;
    l+=region.lat_end[jlat]-region.lat_begin[jlat]+1;

    double y = rg.lat(jlat);
    for( int jlon=region.lat_begin[jlat]; jlon<=region.lat_end[jlat]; ++jlon )
    {
      n = offset_glb[jlat] + jlon;
      double x = rg.lon(jlat,jlon);
      if( stagger && (jlat+1)%2==0 ) x += 180./static_cast<double>(rg.nlon(jlat));

      lonlat(jnode,LON) = x;
      lonlat(jnode,LAT) = y;
      glb_idx(jnode)   = n+1;
      part(jnode) = parts[n];
      Topology::reset(flags(jnode));
      if( jlat == 0 && !include_north_pole)
        Topology::set(flags(jnode),Topology::BC|Topology::NORTH);
      if( jlat == rg.nlat()-1 && !include_south_pole)
        Topology::set(flags(jnode),Topology::BC|Topology::SOUTH);
      if( jlon == 0 && !three_dimensional)
        Topology::set(flags(jnode),Topology::BC|Topology::WEST);
      if( part(jnode) != mypart )
        Topology::set(flags(jnode),Topology::GHOST);

      //Log::info() << "meshgen " << std::setw(2) << glb_idx(jnode) << " ghost = " << Topology::check(flags(jnode),Topology::GHOST) << std::endl;
      ++jnode;
    }
    if( !three_dimensional &&  region.lat_end[jlat]==rg.nlon(jlat)-1 ) // add periodic point
    {
      ++l;
      double x = rg.lon(jlat,rg.nlon(jlat));
      if( stagger && (jlat+1)%2==0 ) x += 180./static_cast<double>(rg.nlon(jlat));

      lonlat(jnode,LON) = x;
      lonlat(jnode,LAT) = y;
      glb_idx(jnode)   = periodic_glb[jlat]+1;
      part(jnode)      = part(jnode-1);
      Topology::reset(flags(jnode));
      Topology::set(flags(jnode),Topology::BC|Topology::EAST);
      Topology::set(flags(jnode),Topology::GHOST);
      ++jnode;
    }
  };

  int jnorth=-1;
  if (include_north_pole)
  {
    jnorth = jnode;
    double y = 90.;
    double x = 180.;
    lonlat(jnode,LON) = x;
    lonlat(jnode,LAT) = y;
    glb_idx(jnode)   = periodic_glb[rg.nlat()-1]+2;
    part(jnode)      = mypart;
    Topology::reset(flags(jnode));
    Topology::set(flags(jnode),Topology::NORTH);
    ++jnode;
  }

  int jsouth=-1;
  if (include_south_pole)
  {
    jsouth = jnode;
    double y = -90.;
    double x =  180.;
    lonlat(jnode,LON) = x;
    lonlat(jnode,LAT) = y;
    glb_idx(jnode)   = periodic_glb[rg.nlat()-1]+3;
    part(jnode)      = mypart;
    Topology::reset(flags(jnode));
    Topology::set(flags(jnode),Topology::SOUTH);
    ++jnode;
  }

  shape = make_shape(nquads,Field::UNDEF_VARS);

  FunctionSpace& quads = mesh.create_function_space( "quads","LagrangeP1",shape );
  quads.metadata().set<long>("type",static_cast<int>(Entity::ELEMS));
  IndexView<int,2> quad_nodes( quads.create_field<int>("nodes",4) );
  ArrayView<gidx_t,1> quad_glb_idx( quads.create_field<gidx_t>("glb_idx",1) );
  ArrayView<int,1> quad_part( quads.create_field<int>("partition",1) );

  shape = make_shape(ntriags,Field::UNDEF_VARS);
  FunctionSpace& triags = mesh.create_function_space( "triags","LagrangeP1",shape );
  triags.metadata().set<long>("type",static_cast<int>(Entity::ELEMS));
  IndexView<int,2> triag_nodes( triags.create_field<int>("nodes",3) );
  ArrayView<gidx_t,1> triag_glb_idx( triags.create_field<gidx_t>("glb_idx",1) );
  ArrayView<int,1> triag_part( triags.create_field<int>("partition",1) );

  /*
  Fill in connectivity tables with global node indices first
  */
  int jquad = 0;
  int jtriag = 0;

  for (int jlat=region.north; jlat<region.south; ++jlat)
  {

    int ilat = jlat-region.north;
    int jlatN = jlat;
    int jlatS = jlat+1;
    int ilatN = ilat;
    int ilatS = ilat+1;
    for (int jelem=0; jelem<region.nb_lat_elems[jlat]; ++jelem)
    {
      const ArrayView<int,1> elem = ArrayView<int,3>(region.elems)[ilat][jelem];

      if(elem[2]>0 && elem[3]>0) // This is a quad
      {
        quad_nodes(jquad,0) = offset_loc[ilatN] + elem[0] - region.lat_begin[jlatN];
        quad_nodes(jquad,1) = offset_loc[ilatS] + elem[1] - region.lat_begin[jlatS];
        quad_nodes(jquad,2) = offset_loc[ilatS] + elem[2] - region.lat_begin[jlatS];
        quad_nodes(jquad,3) = offset_loc[ilatN] + elem[3] - region.lat_begin[jlatN];

        if( three_dimensional )
        {
          if (elem[2] == rg.nlon(jlatS)) quad_nodes(jquad,2) = offset_loc[ilatS];
          if (elem[3] == rg.nlon(jlatN)) quad_nodes(jquad,3) = offset_loc[ilatN];
        }

        // eckit::Log::info(Here())  << quad_nodes(0,jquad) << " " << quad_nodes(1,jquad) << " " << quad_nodes(2,jquad) << " " << quad_nodes(3,jquad) << std::endl;
        quad_glb_idx(jquad) = jquad+jtriag+1;
        quad_part(jquad) = mypart;
        ++jquad;
      }
      else // This is a triag
      {
        if(elem[3]<0) // This is a triangle pointing up
        {
          triag_nodes(jtriag,0) = offset_loc[ilatN] + elem[0] - region.lat_begin[jlatN];
          triag_nodes(jtriag,1) = offset_loc[ilatS] + elem[1] - region.lat_begin[jlatS];
          triag_nodes(jtriag,2) = offset_loc[ilatS] + elem[2] - region.lat_begin[jlatS];
          if( three_dimensional )
          {
            if (elem[0] == rg.nlon(jlatN)) triag_nodes(jtriag,0) = offset_loc[ilatN];
            if (elem[2] == rg.nlon(jlatS)) triag_nodes(jtriag,2) = offset_loc[ilatS];
          }
        }
        else // This is a triangle pointing down
        {
          triag_nodes(jtriag,0) = offset_loc[ilatN] + elem[0] - region.lat_begin[jlatN];
          triag_nodes(jtriag,1) = offset_loc[ilatS] + elem[1] - region.lat_begin[jlatS];
          triag_nodes(jtriag,2) = offset_loc[ilatN] + elem[3] - region.lat_begin[jlatN];
          if( three_dimensional )
          {
            if (elem[1] == rg.nlon(jlatS)) triag_nodes(jtriag,1) = offset_loc[ilatS];
            if (elem[3] == rg.nlon(jlatN)) triag_nodes(jtriag,2) = offset_loc[ilatN];
          }
        }
        triag_glb_idx(jtriag) = jquad+jtriag+1;
        triag_part(jtriag) = mypart;
        ++jtriag;
      }
    }
  }

  if (include_north_pole)
  {
    int jlat = 0;
    int ilat = 0;
    int ip1  = 0;
    for (int ip2=0; ip2<rg.nlon(0); ++ip2)
    {
      int ip3 = ip2+1;
      if( three_dimensional && ip3 == rg.nlon(0) ) ip3 = 0;
      triag_nodes(jtriag,0) = jnorth           + ip1;
      triag_nodes(jtriag,1) = offset_loc[ilat] + ip2;
      triag_nodes(jtriag,2) = offset_loc[ilat] + ip3;
      triag_glb_idx(jtriag) = jquad+jtriag+1;
      triag_part(jtriag) = mypart;
      ++jtriag;
    }
  }
  else if (patch_north_pole )
  {
    int jlat = 0;
    int ilat = 0;
    int ip1, ip2, ip3;
    int q1,q2,q3,q4;

    // start with triag:
    ip1 = 0;
    ip2 = 1;
    ip3 = rg.nlon(0)-1;
    triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
    triag_nodes(jtriag,1) = offset_loc[ilat] + ip2;
    triag_nodes(jtriag,2) = offset_loc[ilat] + ip3;
    triag_glb_idx(jtriag) = jquad+jtriag+1;
    triag_part(jtriag) = mypart;
    ++jtriag;

    q1 = ip2;
    q4 = ip3;
    for ( int k=0; k<(rg.nlon(jlat)-4)/2; ++k )
    {
      q2=q1+1;
      q3=q4-1;

      // add triag
      ip1 = q1;
      ip2 = q3;
      ip3 = q4;
      triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
      triag_nodes(jtriag,1) = offset_loc[ilat] + ip2;
      triag_nodes(jtriag,2) = offset_loc[ilat] + ip3;
      triag_glb_idx(jtriag) = jquad+jtriag+1;
      triag_part(jtriag) = mypart;
      ++jtriag;

      // add triag
      ip1 = q1;
      ip2 = q2;
      ip3 = q3;
      triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
      triag_nodes(jtriag,1) = offset_loc[ilat] + ip2;
      triag_nodes(jtriag,2) = offset_loc[ilat] + ip3;
      triag_glb_idx(jtriag) = jquad+jtriag+1;
      triag_part(jtriag) = mypart;
      ++jtriag;

      q1 = q2;
      q4 = q3;
    }
    // end with triag:
    ip1 = q1;
    ip2 = q1+1;
    ip3 = q4;
    triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
    triag_nodes(jtriag,1) = offset_loc[ilat] + ip2;
    triag_nodes(jtriag,2) = offset_loc[ilat] + ip3;
    triag_glb_idx(jtriag) = jquad+jtriag+1;
    triag_part(jtriag) = mypart;
    ++jtriag;
  }

  if (include_south_pole)
  {
    int jlat = rg.nlat()-1;
    int ilat = region.south-region.north;
    int ip1 = 0;
    for (int ip2=1; ip2<rg.nlon(jlat)+1; ++ip2)
    {
      int ip3 = ip2-1;
      triag_nodes(jtriag,0) = jsouth           + ip1;
      triag_nodes(jtriag,1) = offset_loc[ilat] + ip2;
      triag_nodes(jtriag,2) = offset_loc[ilat] + ip3;
      if( three_dimensional && ip2 == rg.nlon(jlat) )
        triag_nodes(jtriag,1) = offset_loc[ilat] + 0;

      triag_glb_idx(jtriag) = jquad+jtriag+1;
      triag_part(jtriag) = mypart;
      ++jtriag;
    }
  }
  else if (patch_south_pole)
  {
    int jlat = rg.nlat()-1;
    int ilat = region.south-region.north;
    int ip1, ip2, ip3;
    int q1,q2,q3,q4;

    // start with triag:
    ip1 = 0;
    ip2 = 1;
    ip3 = rg.nlon(0)-1;
    triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
    triag_nodes(jtriag,2) = offset_loc[ilat] + ip2;
    triag_nodes(jtriag,1) = offset_loc[ilat] + ip3;
    triag_glb_idx(jtriag) = jquad+jtriag+1;
    triag_part(jtriag) = mypart;
    ++jtriag;

    q1 = ip2;
    q4 = ip3;
    for ( int k=0; k<(rg.nlon(jlat)-4)/2; ++k )
    {
      q2=q1+1;
      q3=q4-1;

      // add triag
      ip1 = q1;
      ip2 = q3;
      ip3 = q4;
      triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
      triag_nodes(jtriag,2) = offset_loc[ilat] + ip2;
      triag_nodes(jtriag,1) = offset_loc[ilat] + ip3;
      triag_glb_idx(jtriag) = jquad+jtriag+1;
      triag_part(jtriag) = mypart;
      ++jtriag;

      // add triag
      ip1 = q1;
      ip2 = q2;
      ip3 = q3;
      triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
      triag_nodes(jtriag,2) = offset_loc[ilat] + ip2;
      triag_nodes(jtriag,1) = offset_loc[ilat] + ip3;
      triag_glb_idx(jtriag) = jquad+jtriag+1;
      triag_part(jtriag) = mypart;
      ++jtriag;

      q1 = q2;
      q4 = q3;
    }
    // end with triag:
    ip1 = q1;
    ip2 = q1+1;
    ip3 = q4;
    triag_nodes(jtriag,0) = offset_loc[ilat] + ip1;
    triag_nodes(jtriag,2) = offset_loc[ilat] + ip2;
    triag_nodes(jtriag,1) = offset_loc[ilat] + ip3;
    triag_glb_idx(jtriag) = jquad+jtriag+1;
    triag_part(jtriag) = mypart;
    ++jtriag;
  }

  nodes.metadata().set("nb_owned",nnodes);
  quads.metadata().set("nb_owned",nquads);
  triags.metadata().set("nb_owned",ntriags);

  gidx_t max_glb_idx = rg.npts()+rg.nlat();
  if( three_dimensional ) max_glb_idx -= rg.nlat();
  if( include_north_pole ) max_glb_idx += 1;
  if( include_south_pole ) max_glb_idx += 1;
  nodes.metadata().set("max_glb_idx",max_glb_idx);
  quads.metadata().set("max_glb_idx", nquads+ntriags);
  triags.metadata().set("max_glb_idx",nquads+ntriags);

  generate_global_element_numbering( mesh );


  ///debug
  //  ArrayView<double,2> xyz( mesh.function_space("nodes").create_field<double>("xyz",3,IF_EXISTS_RETURN) );
  //  for( int jnode=0; jnode<lonlat.shape(0); ++jnode )
  //  {
  //    eckit::geometry::lonlat_to_3d( lonlat[jnode].data(), xyz[jnode].data() );
  //  }

  //  io::Gmsh gmsh;
  //  if(three_dimensional)
  //    gmsh.options.set("nodes",std::string("xyz"));
  //  gmsh.write(mesh,"/tmp/debug/mesh.msh");
}

void ReducedGridMeshGenerator::generate_global_element_numbering( Mesh& mesh ) const
{
  int loc_nb_elems = 0;
  std::vector<int> elem_counts( eckit::mpi::size() );
  std::vector<int> elem_displs( eckit::mpi::size() );
  for( int f=0; f<mesh.nb_function_spaces(); ++f )
  {
    FunctionSpace& elements = mesh.function_space(f);
    if( elements.metadata().get<long>("type") == Entity::ELEMS )
    {
      loc_nb_elems += elements.shape(0);
    }
  }
  ECKIT_MPI_CHECK_RESULT( MPI_Allgather( &loc_nb_elems, 1, MPI_INT,
                                   elem_counts.data(), 1, MPI_INT, eckit::mpi::comm()) );
  elem_displs[0] = 0;
  for( int jpart=1; jpart<eckit::mpi::size(); ++jpart )
  {
    elem_displs[jpart] = elem_displs[jpart-1] + elem_counts[jpart-1];
  }

  gidx_t gid = 1+elem_displs[ eckit::mpi::rank() ];

  for( int f=0; f<mesh.nb_function_spaces(); ++f )
  {
    FunctionSpace& elements = mesh.function_space(f);
    if( elements.metadata().get<long>("type") == Entity::ELEMS )
    {
      ArrayView<gidx_t,1> glb_idx( elements.field("glb_idx") );
      int nb_elems = elements.shape(0);
      for( int e=0; e<nb_elems; ++e )
      {
        glb_idx(e) = gid++;
      }
    }
  }
}

namespace {
static MeshGeneratorBuilder< ReducedGridMeshGenerator > __reducedgrid("ReducedGrid");
}

} // namespace meshgen
} // namespace atlas

