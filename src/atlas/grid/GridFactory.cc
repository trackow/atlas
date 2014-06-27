/*
 * (C) Copyright 1996-2014 ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */

#include "atlas/grid/GridFactory.h"

//------------------------------------------------------------------------------------------------------

namespace atlas {
namespace grid {

//------------------------------------------------------------------------------------------------------

Grid::Ptr GridFactory::create(const GridSpec& grid_spec)
{
   std::map<std::string, GridCreator*>::iterator i = get_table().find(grid_spec.grid_type());

   if (i != get_table().end()) {
      Grid::Ptr the_grid = i->second->create();
      the_grid->constructFrom(grid_spec);
      return the_grid;
   }

   return Grid::Ptr();
}

void GridFactory::registerit(const std::string& grid_type, GridCreator* creator)
{
   get_table()[grid_type] = creator;
}

std::map<std::string, GridCreator*>& GridFactory::get_table()
{
   static std::map<std::string, GridCreator*> table;
   return table;
}

// ===============================================================================================

// have the creator's constructor do the registration
GridCreator::GridCreator(const std::string& grid_type)
{
   GridFactory::registerit(grid_type, this);
}

GridCreator::~GridCreator() {}

//------------------------------------------------------------------------------------------------------

} // namespace grid
} // namespace atlas