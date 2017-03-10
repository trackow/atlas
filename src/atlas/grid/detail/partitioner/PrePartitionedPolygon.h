/*
 * (C) Copyright 1996-2016 ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */


#pragma once

#include "PartitionerFromPrePartitionedMesh.h"

namespace atlas {
namespace grid {
namespace detail {
namespace partitioner {


class PrePartitionedPolygon : public PartitionerFromPrePartitionedMesh {
public:

    PrePartitionedPolygon(const Grid& grid) : PartitionerFromPrePartitionedMesh(grid) {}
    PrePartitionedPolygon(const Grid& grid, const size_t nb_partitions) : PartitionerFromPrePartitionedMesh(grid, nb_partitions) {}

    PrePartitionedPolygon(const Grid& grid,
      const mesh::Mesh& mesh ) :
      PartitionerFromPrePartitionedMesh(grid,mesh) {}

    /**
     * @brief Create a GridDistribution, placing nodes in the same partitions as a
     * given pre-partitioned mesh. The method reconstructs the partition edges polygon
     * and tests every target grid node if it is internal to the polygon.
     * @param gridTarget grid to be distributed
     * @param meshSource mesh already partitioned
     * @return grid partitioner
     */
    void partition( int part[] ) const;

};


}  // partitioner
}  // detail
}  // grid
}  // atlas
