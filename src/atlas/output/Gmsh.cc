/*
 * (C) Copyright 1996-2017 ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */

#include <map>
#include <string>

#include "atlas/field/Field.h"
#include "atlas/field/FieldSet.h"
#include "atlas/functionspace/FunctionSpace.h"
#include "atlas/mesh/actions/BuildXYZField.h"
#include "atlas/mesh/Mesh.h"
#include "atlas/mesh/Nodes.h"
#include "atlas/output/Gmsh.h"
#include "atlas/runtime/ErrorHandling.h"
#include "atlas/runtime/Log.h"
#include "atlas/util/io/Gmsh.h"
#include "eckit/exception/Exceptions.h"

using atlas::field::Field;
using atlas::field::FieldSet;
using atlas::mesh::Mesh;
using atlas::functionspace::FunctionSpace;
using eckit::Parametrisation;

namespace atlas {
namespace output {

// -----------------------------------------------------------------------------

std::string GmshFileStream::parallelPathName(const PathName& path,int part)
{
  std::stringstream s;
  // s << path.dirName() << "/" << path.baseName(false) << "_p" << part << ".msh";
  s << path.asString() << ".p"<<part;
  return s.str();
}

// -----------------------------------------------------------------------------

GmshFileStream::GmshFileStream(const PathName& file_path, const char* mode, int part)
{
  PathName par_path(file_path);
  std::ios_base::openmode omode = std::ios_base::out;
  if     ( std::string(mode)=="w" )  omode = std::ios_base::out;
  else if( std::string(mode)=="a" )  omode = std::ios_base::app;

  if( part<0 || parallel::mpi::comm().size() == 1 )
  {
    std::ofstream::open(file_path.localPath(), omode);
  }
  else
  {
    if (parallel::mpi::comm().rank() == 0)
    {
      PathName par_path(file_path);
      std::ofstream par_file(par_path.localPath(), std::ios_base::out);
      for(size_t p = 0; p < parallel::mpi::comm().size(); ++p)
      {
        par_file << "Merge \"" << parallelPathName(file_path,p) << "\";" << std::endl;
      }
      par_file.close();
    }
    PathName path( parallelPathName(file_path,part) );
    std::ofstream::open(path.localPath(), omode);
  }
}

// -----------------------------------------------------------------------------

void Gmsh::defaults()
{
  config_.binary = false;
  config_.nodes = "lonlat";
  config_.gather = false;
  config_.ghost = false;
  config_.elements = true;
  config_.edges = false;
  config_.levels.clear();
  config_.file = "output.msh";
  config_.info = false;
  config_.openmode = "w";
  config_.coordinates = "lonlat";
}

// -----------------------------------------------------------------------------

namespace /*anonymous*/ {

// -----------------------------------------------------------------------------

void merge(Gmsh::Configuration& present, const eckit::Parametrisation& update)
{
  update.get("binary",present.binary);
  update.get("nodes",present.nodes);
  update.get("gather",present.gather);
  update.get("ghost",present.ghost);
  update.get("elements",present.elements);
  update.get("edges",present.edges);
  update.get("levels",present.levels);
  update.get("file",present.file);
  update.get("info",present.info);
  update.get("openmode",present.openmode);
  update.get("coordinates",present.coordinates);
}

// -----------------------------------------------------------------------------

util::io::Gmsh writer(const Gmsh::Configuration& c)
{
  util::io::Gmsh gmsh;
  Gmsh::setGmshConfiguration(gmsh,c);
  return gmsh;
}

// -----------------------------------------------------------------------------

std::ios_base::openmode openmode(const Gmsh::Configuration& c)
{
  std::ios_base::openmode omode(std::ios_base::out);
  if     ( std::string(c.openmode)=="w" )  omode = std::ios_base::out;
  else if( std::string(c.openmode)=="a" )  omode = std::ios_base::app;
  if( c.binary )                           omode |= std::ios::binary;
  return omode;
}

// -----------------------------------------------------------------------------

} // anonymous namespace

// -----------------------------------------------------------------------------

void Gmsh::setGmshConfiguration(util::io::Gmsh& gmsh, const Gmsh::Configuration& c)
{
  gmsh.options.set("ascii", not c.binary);
  gmsh.options.set("nodes",c.nodes);
  gmsh.options.set("gather",c.gather);
  gmsh.options.set("ghost",c.ghost);
  gmsh.options.set("elements",c.elements);
  gmsh.options.set("edges",c.edges);
  gmsh.options.set("levels",c.levels);
  gmsh.options.set("info",c.info);
  gmsh.options.set("nodes",c.coordinates);
}

// -----------------------------------------------------------------------------

Gmsh::Gmsh(Stream& stream)
{
  defaults();
  NOTIMP;
}

// -----------------------------------------------------------------------------

Gmsh::Gmsh(Stream& stream,const eckit::Parametrisation& config)
{
  defaults();
  merge(config_,config);
  NOTIMP;
}

// -----------------------------------------------------------------------------

Gmsh::Gmsh(const PathName& file, const std::string& mode)
{
  defaults();
  config_.file = file.asString();
  config_.openmode = std::string(mode);
}

// -----------------------------------------------------------------------------

Gmsh::Gmsh(const PathName& file, const std::string& mode, const eckit::Parametrisation& config)
{
  defaults();
  merge(config_,config);
  config_.file = file.asString();
  config_.openmode = std::string(mode);
}

// -----------------------------------------------------------------------------

Gmsh::Gmsh(const PathName& file)
{
  defaults();
  config_.file = file.asString();
}

// -----------------------------------------------------------------------------

Gmsh::Gmsh(const PathName& file, const eckit::Parametrisation& config)
{
  defaults();
  merge(config_,config);
  config_.file = file.asString();
}

// -----------------------------------------------------------------------------

Gmsh::~Gmsh()
{
}

// -----------------------------------------------------------------------------

void Gmsh::write(
        const mesh::Mesh& mesh,
        const eckit::Parametrisation& config) const
{
  Gmsh::Configuration c = config_;
  merge(c,config);

  if( c.coordinates == "xyz" and not mesh.nodes().has_field("xyz") )
  {
      Log::debug<ATLAS>() << "Building xyz representation for nodes" << std::endl;
      mesh::actions::BuildXYZField("xyz")(const_cast<mesh::Mesh&>(mesh));
  }

  writer(c).write(mesh,c.file);
  config_.openmode = "a";
}

// -----------------------------------------------------------------------------

void Gmsh::write(
    const field::Field& field,
    const eckit::Parametrisation& config ) const
{
  Gmsh::Configuration c = config_;
  merge(c,config);
  writer(c).write(field,c.file,openmode(c));
  config_.openmode = "a";
}

// -----------------------------------------------------------------------------

void Gmsh::write(
    const field::FieldSet& fields,
    const eckit::Parametrisation& config) const
{
  Gmsh::Configuration c = config_;
  merge(c,config);
  writer(c).write(fields,fields.field(0).functionspace(),c.file,openmode(c));
  config_.openmode = "a";
}

// -----------------------------------------------------------------------------

void Gmsh::write(
    const field::Field& field,
    const functionspace::FunctionSpace& functionspace,
    const eckit::Parametrisation& config) const
{
  Gmsh::Configuration c = config_;
  merge(c,config);
  writer(c).write(field,functionspace,c.file,openmode(c));
  config_.openmode = "a";
}

// -----------------------------------------------------------------------------

void Gmsh::write(
    const field::FieldSet& fields,
    const functionspace::FunctionSpace& functionspace,
    const eckit::Parametrisation& config) const
{
  Gmsh::Configuration c = config_;
  merge(c,config);
  writer(c).write(fields,functionspace,c.file,openmode(c));
  config_.openmode = "a";
}

// -----------------------------------------------------------------------------

extern "C" {

Gmsh* atlas__output__Gmsh__create_pathname_mode(const char* pathname, const char* mode)
{
  Gmsh* gmsh(0);
  ATLAS_ERROR_HANDLING( gmsh = new Gmsh(std::string(pathname),std::string(mode) ) );
  return gmsh;
}
Gmsh* atlas__output__Gmsh__create_pathname_mode_config(const char* pathname, const char* mode, const Parametrisation* params)
{
  Gmsh* gmsh(0);
  ATLAS_ERROR_HANDLING( gmsh = new Gmsh(std::string(pathname),std::string(mode), *params ) );
  return gmsh;
}
}

namespace {
static OutputBuilder< Gmsh > __gmsh("gmsh");
}

} // namespace output
} // namespace atlas

