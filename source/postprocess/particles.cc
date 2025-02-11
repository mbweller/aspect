/*
  Copyright (C) 2011 - 2016 by the authors of the ASPECT code.

 This file is part of ASPECT.

 ASPECT is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2, or (at your option)
 any later version.

 ASPECT is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with ASPECT; see the file LICENSE.  If not see
 <http://www.gnu.org/licenses/>.
 */

#include <aspect/global.h>
#include <aspect/postprocess/particles.h>
#include <aspect/utilities.h>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

#include <stdio.h>
#include <unistd.h>

namespace aspect
{
  namespace Postprocess
  {
#if DEAL_II_VERSION_GTE(9,0,0)
    namespace internal
    {
      template<int dim>
      void
      ParticleOutput<dim>::build_patches(const std::multimap<aspect::Particle::types::LevelInd, aspect::Particle::Particle<dim> > &particles,
                                         const aspect::Particle::Property::ParticlePropertyInformation &property_information)
      {
        // First store the names of the data fields
        dataset_names.reserve(property_information.n_components()+1);
        dataset_names.push_back("id");

        for (unsigned int field_index = 0; field_index < property_information.n_fields(); ++field_index)
          {
            const unsigned n_components = property_information.get_components_by_field_index(field_index);
            const std::string field_name = property_information.get_field_name_by_index(field_index);
            // If it is a 1D element, or a vector, print just the name, otherwise append the index after an underscore
            if ((n_components == 1) || (n_components == dim))
              for (unsigned int component_index=0; component_index<n_components; ++component_index)
                dataset_names.push_back(field_name);
            else
              for (unsigned int component_index=0; component_index<n_components; ++component_index)
                dataset_names.push_back(field_name + "_" + Utilities::to_string(component_index));
          }


        // Second store which of these data fields are vectors

        for (unsigned int field_index = 0; field_index < property_information.n_fields(); ++field_index)
          {
            const unsigned n_components = property_information.get_components_by_field_index(field_index);

            // If the property has dim components, we treat it as vector
            if (n_components == dim)
              {
                const unsigned int field_position = property_information.get_position_by_field_index(field_index);
                const std::string field_name = property_information.get_field_name_by_index(field_index);
                vector_datasets.push_back(std::make_tuple(field_position+1,
                                                          field_position+n_components,
                                                          field_name));
              }
          }


        // Third build the actual patch data
        patches.resize(particles.size());

        typename std::multimap<aspect::Particle::types::LevelInd, aspect::Particle::Particle<dim> >::const_iterator particle = particles.begin();

        for (unsigned int i=0; particle != particles.end(); ++particle, ++i)
          {
            patches[i].vertices[0] = particle->second.get_location();
            patches[i].patch_index = i;
            patches[i].n_subdivisions = 1;
            patches[i].data.reinit(property_information.n_components()+1,1);

            patches[i].data(0,0) = particle->second.get_id();

            const ArrayView<const double> properties = particle->second.get_properties();

            for (unsigned int property_index = 0; property_index < properties.size(); ++property_index)
              patches[i].data(property_index+1,0) = properties[property_index];
          }
      }

      template <int dim>
      const std::vector<DataOutBase::Patch<0,dim> > &
      ParticleOutput<dim>::get_patches () const
      {
        return patches;
      }

      template <int dim>
      std::vector< std::string >
      ParticleOutput<dim>::get_dataset_names () const
      {
        return dataset_names;
      }

      template <int dim>
      std::vector<std_cxx11::tuple<unsigned int, unsigned int, std::string> >
      ParticleOutput<dim>::get_vector_data_ranges () const
      {
        return vector_datasets;
      }

    }
#endif

    template <int dim>
    Particles<dim>::Particles ()
      :
      // the following value is later read from the input file
      output_interval (0),
      // initialize this to a nonsensical value; set it to the actual time
      // the first time around we get to check it
      last_output_time (std::numeric_limits<double>::quiet_NaN())
#if DEAL_II_VERSION_GTE(9,0,0)
      ,output_file_number (numbers::invalid_unsigned_int),
      group_files(0),
      write_in_background_thread(false)
#endif
    {}

    template <int dim>
    Particles<dim>::~Particles ()
    {
#if DEAL_II_VERSION_GTE(9,0,0)
      // make sure a thread that may still be running in the background,
      // writing data, finishes
      background_thread.join ();
#endif
    }

    template <int dim>
    void
    Particles<dim>::generate_particles()
    {
      world.generate_particles();
    }

    template <int dim>
    void
    Particles<dim>::initialize_particles()
    {
      world.initialize_particles();
    }

    template <int dim>
    const Particle::World<dim> &
    Particles<dim>::get_particle_world() const
    {
      return world;
    }

    template <int dim>
    Particle::World<dim> &
    Particles<dim>::get_particle_world()
    {
      return world;
    }

#if DEAL_II_VERSION_GTE(9,0,0)
    template <int dim>
    void Particles<dim>::writer (const std::string filename,
                                 const std::string temporary_output_location,
                                 const std::string *file_contents)
    {
      std::string tmp_filename = filename;
      if (temporary_output_location != "")
        {
          tmp_filename = temporary_output_location + "/aspect.tmp.XXXXXX";

          // Create the temporary file; get at the actual filename
          // by using a C-style string that mkstemp will then overwrite
          char *tmp_filename_x = new char[tmp_filename.size()+1];
          std::strcpy(tmp_filename_x, tmp_filename.c_str());
          int tmp_file_desc = mkstemp(tmp_filename_x);
          tmp_filename = tmp_filename_x;
          delete []tmp_filename_x;

          // If we failed to create the temp file, just write directly to the target file.
          // We also provide a warning about this fact. There are places where
          // this fails *on every node*, so we will get a lot of warning messages
          // into the output; in these cases, just writing multiple pieces to
          // std::cerr will produce an unreadable mass of text; rather, first
          // assemble the error message completely, and then output it atomically
          if (tmp_file_desc == -1)
            {
              const std::string x = ("***** WARNING: could not create temporary file <"
                                     +
                                     tmp_filename
                                     +
                                     ">, will output directly to final location. This may negatively "
                                     "affect performance. (On processor "
                                     + Utilities::int_to_string(Utilities::MPI::this_mpi_process (MPI_COMM_WORLD))
                                     + ".)\n");

              std::cerr << x << std::flush;

              tmp_filename = filename;
            }
          else
            close(tmp_file_desc);
        }

      std::ofstream out(tmp_filename.c_str());

      AssertThrow (out, ExcMessage(std::string("Trying to write to file <") +
                                   filename +
                                   ">, but the file can't be opened!"))

      // now write and then move the tmp file to its final destination
      // if necessary
      out << *file_contents;
      out.close ();

      if (tmp_filename != filename)
        {
          std::string command = std::string("mv ") + tmp_filename + " " + filename;
          int error = system(command.c_str());

          AssertThrow(error == 0,
                      ExcMessage("Could not move " + tmp_filename + " to "
                                 + filename + ". On processor "
                                 + Utilities::int_to_string(Utilities::MPI::this_mpi_process (MPI_COMM_WORLD)) + "."));
        }

      // destroy the pointer to the data we needed to write
      delete file_contents;
    }

    template <int dim>
    void
    Particles<dim>::write_master_files (const internal::ParticleOutput<dim> &data_out,
                                        const std::string &solution_file_prefix,
                                        const std::vector<std::string> &filenames)
    {
      const double time_in_years_or_seconds = (this->convert_output_to_years() ?
                                               this->get_time() / year_in_seconds :
                                               this->get_time());
      const std::string
      pvtu_master_filename = (solution_file_prefix +
                              ".pvtu");
      std::ofstream pvtu_master ((this->get_output_directory() + "particles/" +
                                  pvtu_master_filename).c_str());
      data_out.write_pvtu_record (pvtu_master, filenames);

      // now also generate a .pvd file that matches simulation
      // time and corresponding .pvtu record
      times_and_pvtu_file_names.push_back(std::make_pair
                                          (time_in_years_or_seconds, "particles/"+pvtu_master_filename));

      const std::string
      pvd_master_filename = (this->get_output_directory() + "particles.pvd");
      std::ofstream pvd_master (pvd_master_filename.c_str());

      DataOutBase::write_pvd_record (pvd_master, times_and_pvtu_file_names);

      // finally, do the same for Visit via the .visit file for this
      // time step, as well as for all time steps together
      const std::string
      visit_master_filename = (this->get_output_directory()
                               + "particles/"
                               + solution_file_prefix
                               + ".visit");
      std::ofstream visit_master (visit_master_filename.c_str());

      DataOutBase::write_visit_record (visit_master, filenames);

      {
        // the global .visit file needs the relative path because it sits a
        // directory above
        std::vector<std::string> filenames_with_path;
        for (std::vector<std::string>::const_iterator it = filenames.begin();
             it != filenames.end();
             ++it)
          {
            filenames_with_path.push_back("particles/" + (*it));
          }

        output_file_names_by_timestep.push_back (filenames_with_path);
      }

      std::ofstream global_visit_master ((this->get_output_directory() +
                                          "particles.visit").c_str());

      std::vector<std::pair<double, std::vector<std::string> > > times_and_output_file_names;
      for (unsigned int timestep=0; timestep<times_and_pvtu_file_names.size(); ++timestep)
        times_and_output_file_names.push_back(std::make_pair(times_and_pvtu_file_names[timestep].first,
                                                             output_file_names_by_timestep[timestep]));
      DataOutBase::write_visit_record (global_visit_master, times_and_output_file_names);
    }
#endif

    template <int dim>
    std::pair<std::string,std::string>
    Particles<dim>::execute (TableHandler &statistics)
    {
      // if this is the first time we get here, set the last output time
      // to the current time - output_interval. this makes sure we
      // always produce data during the first time step
      if (std::isnan(last_output_time))
        last_output_time = this->get_time() - output_interval;

      // Do not advect the particles in the initial refinement stage
      const bool in_initial_refinement = (this->get_timestep_number() == 0)
                                         && (this->get_pre_refinement_step() < this->get_parameters().initial_adaptive_refinement);
      if (!in_initial_refinement)
        // Advance the particles in the world to the current time
        world.advance_timestep();

      statistics.add_value("Number of advected particles",world.n_global_particles());

      // If it's not time to generate an output file or we do not write output
      // return early with the number of particles that were advected
      if (this->get_time() < last_output_time + output_interval)
        return std::make_pair("Number of advected particles:",
                              Utilities::int_to_string(world.n_global_particles()));


      if (world.get_property_manager().need_update() == Particle::Property::update_output_step)
        world.update_particles();

#if DEAL_II_VERSION_GTE(9,0,0)
      if (output_file_number == numbers::invalid_unsigned_int)
        output_file_number = 0;
      else
        ++output_file_number;

      // Create the particle output
      internal::ParticleOutput<dim> data_out;
      data_out.build_patches(world.get_particles(),
                             world.get_property_manager().get_data_info());

      // Now prepare everything for writing the output and choose output format
      std::string particle_file_prefix = "particles-" + Utilities::int_to_string (output_file_number, 5);

      const double time_in_years_or_seconds = (this->convert_output_to_years() ?
                                               this->get_time() / year_in_seconds :
                                               this->get_time());

      for (std::vector<std::string>::iterator output_format = output_formats.begin();
           output_format != output_formats.end();
           ++output_format)
        {
          if (*output_format == "none")
            {
              // If we do not write output return early with the number of advected particles
              return std::make_pair("Number of advected particles:",
                                    Utilities::int_to_string(world.n_global_particles()));
            }
          else if (*output_format=="hdf5")
            {
              const std::string particle_file_name = "particles/" + particle_file_prefix + ".h5";
              const std::string xdmf_filename = "particles.xdmf";

              // Do not filter redundant values, there are no duplicate particles
              DataOutBase::DataOutFilter data_filter(DataOutBase::DataOutFilterFlags(false, true));

              data_out.write_filtered_data(data_filter);
              data_out.write_hdf5_parallel(data_filter,
                                           this->get_output_directory()+particle_file_name,
                                           this->get_mpi_communicator());

              const XDMFEntry new_xdmf_entry = data_out.create_xdmf_entry(data_filter,
                                                                          particle_file_name,
                                                                          time_in_years_or_seconds,
                                                                          this->get_mpi_communicator());
              xdmf_entries.push_back(new_xdmf_entry);
              data_out.write_xdmf_file(xdmf_entries, this->get_output_directory() + xdmf_filename,
                                       this->get_mpi_communicator());
            }
          else if (*output_format == "vtu")
            {
              // Write master files (.pvtu,.pvd,.visit) on the master process
              const int my_id = Utilities::MPI::this_mpi_process(this->get_mpi_communicator());

              if (my_id == 0)
                {
                  std::vector<std::string> filenames;
                  const unsigned int n_processes = Utilities::MPI::n_mpi_processes(this->get_mpi_communicator());
                  const unsigned int n_files = (group_files == 0) ? n_processes : std::min(group_files,n_processes);
                  for (unsigned int i=0; i<n_files; ++i)
                    filenames.push_back (particle_file_prefix
                                         + "." + Utilities::int_to_string(i, 4)
                                         + ".vtu");
                  write_master_files (data_out, particle_file_prefix, filenames);
                }

              const unsigned int n_processes = Utilities::MPI::n_mpi_processes(this->get_mpi_communicator());

              const unsigned int my_file_id = (group_files == 0
                                               ?
                                               my_id
                                               :
                                               my_id % group_files);
              const std::string filename = this->get_output_directory()
                                           + "particles/"
                                           + particle_file_prefix
                                           + "."
                                           + Utilities::int_to_string (my_file_id, 4)
                                           + ".vtu";

              // pass time step number and time as metadata into the output file
              DataOutBase::VtkFlags vtk_flags;
              vtk_flags.cycle = this->get_timestep_number();
              vtk_flags.time = time_in_years_or_seconds;

              data_out.set_flags (vtk_flags);

              // Write as many files as processes. For this case we support writing in a
              // background thread and to a temporary location, so we first write everything
              // into a string that is written to disk in a writer function
              if ((group_files == 0) || (group_files >= n_processes))
                {
                  // Put the content we want to write into a string object that
                  // we can then write in the background
                  const std::string *file_contents;
                  {
                    std::ostringstream tmp;

                    data_out.write (tmp, DataOutBase::parse_output_format(*output_format));
                    file_contents = new std::string (tmp.str());
                  }

                  if (write_in_background_thread)
                    {
                      // Wait for all previous write operations to finish, should
                      // any be still active,
                      background_thread.join ();

                      // then continue with writing our own data.
                      background_thread = Threads::new_thread (&writer,
                                                               filename,
                                                               temporary_output_location,
                                                               file_contents);
                    }
                  else
                    writer(filename,temporary_output_location,file_contents);
                }
              // Just write one data file in parallel
              else if (group_files == 1)
                {
                  data_out.write_vtu_in_parallel(filename.c_str(),
                                                 this->get_mpi_communicator());
                }
              // Write as many output files as 'group_files' groups
              else
                {
                  int color = my_id % group_files;

                  MPI_Comm comm;
                  MPI_Comm_split(this->get_mpi_communicator(), color, my_id, &comm);

                  data_out.write_vtu_in_parallel(filename.c_str(), comm);
                  MPI_Comm_free(&comm);
                }
            }
          // Write in a different format than hdf5 or vtu. This case is supported, but is not
          // optimized for parallel output in that every process will write one file directly
          // into the output directory. This may or may not affect performance depending on
          // the model setup and the network file system type.
          else
            {
              const unsigned int myid = Utilities::MPI::this_mpi_process(this->get_mpi_communicator());

              const std::string filename = this->get_output_directory()
                                           + "particles/"
                                           + particle_file_prefix
                                           + "."
                                           +  Utilities::int_to_string (myid, 4)
                                           + DataOutBase::default_suffix
                                           (DataOutBase::parse_output_format(*output_format));

              std::ofstream out (filename.c_str());

              AssertThrow(out,
                          ExcMessage("Unable to open file for writing: " + filename +"."));

              data_out.write (out, DataOutBase::parse_output_format(*output_format));
            }
        }


      // up the next time we need output
      set_last_output_time (this->get_time());

      const std::string particle_output = this->get_output_directory() + "particles/" + particle_file_prefix;

      // record the file base file name in the output file
      statistics.add_value ("Particle file name",
                            particle_output);
      return std::make_pair("Writing particle output:", particle_output);
#else
      set_last_output_time (this->get_time());
      const std::string data_file_name = world.generate_output();

      // If we do not write output return early with the number of particles
      // that were advected
      if (data_file_name == "")
        return std::make_pair("Number of advected particles:",
                              Utilities::int_to_string(world.n_global_particles()));

      // record the file base file name in the output file
      statistics.add_value ("Particle file name",
                            this->get_output_directory() + data_file_name);
      return std::make_pair("Writing particle output:", data_file_name);
#endif
    }



    template <int dim>
    void
    Particles<dim>::set_last_output_time (const double current_time)
    {
      // if output_interval is positive, then update the last supposed output
      // time
      if (output_interval > 0)
        {
          // We need to find the last time output was supposed to be written.
          // this is the last_output_time plus the largest positive multiple
          // of output_intervals that passed since then. We need to handle the
          // edge case where last_output_time+output_interval==current_time,
          // we did an output and std::floor sadly rounds to zero. This is done
          // by forcing std::floor to round 1.0-eps to 1.0.
          const double magic = 1.0+2.0*std::numeric_limits<double>::epsilon();
          last_output_time = last_output_time + std::floor((current_time-last_output_time)/output_interval*magic) * output_interval/magic;
        }
    }


    template <int dim>
    template <class Archive>
    void Particles<dim>::serialize (Archive &ar, const unsigned int)
    {
      ar &last_output_time
#if DEAL_II_VERSION_GTE(9,0,0)
      & output_file_number
      & times_and_pvtu_file_names
      & output_file_names_by_timestep
      & xdmf_entries
#endif
      ;
    }


    template <int dim>
    void
    Particles<dim>::save (std::map<std::string, std::string> &status_strings) const
    {
      std::ostringstream os;
      aspect::oarchive oa (os);

      world.save(os);
      oa << (*this);

      status_strings["Particles"] = os.str();
    }


    template <int dim>
    void
    Particles<dim>::load (const std::map<std::string, std::string> &status_strings)
    {
      // see if something was saved
      if (status_strings.find("Particles") != status_strings.end())
        {
          std::istringstream is (status_strings.find("Particles")->second);
          aspect::iarchive ia (is);

          // Load the particle world
          world.load(is);

          ia >> (*this);
        }
    }


    template <int dim>
    void
    Particles<dim>::declare_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection("Postprocess");
      {
        prm.enter_subsection("Particles");
        {
          prm.declare_entry ("Time between data output", "1e8",
                             Patterns::Double (0),
                             "The time interval between each generation of "
                             "output files. A value of zero indicates that "
                             "output should be generated every time step.\n\n"
                             "Units: years if the "
                             "'Use years in output instead of seconds' parameter is set; "
                             "seconds otherwise.");

#if DEAL_II_VERSION_GTE(9,0,0)
          // now also see about the file format we're supposed to write in
          // Note: "ascii" is a legacy format used by ASPECT before particle output
          // in deal.II was implemented. It is nearly identical to the gnuplot format, thus
          // we now simply replace "ascii" by "gnuplot" should it be selected.
          prm.declare_entry ("Data output format", "vtu",
                             Patterns::MultipleSelection (DataOutBase::get_output_format_names ()+"|ascii"),
                             "A comma seperated list of file formats to be used for graphical "
                             "output.");

          prm.declare_entry ("Number of grouped files", "16",
                             Patterns::Integer(0),
                             "VTU file output supports grouping files from several CPUs "
                             "into a given number of files using MPI I/O when writing on a parallel "
                             "filesystem. Select 0 for no grouping. This will disable "
                             "parallel file output and instead write one file per processor. "
                             "A value of 1 will generate one big file containing the whole "
                             "solution, while a larger value will create that many files "
                             "(at most as many as there are MPI ranks).");

          prm.declare_entry ("Write in background thread", "false",
                             Patterns::Bool(),
                             "File operations can potentially take a long time, blocking the "
                             "progress of the rest of the model run. Setting this variable to "
                             "`true' moves this process into a background thread, while the "
                             "rest of the model continues.");

          prm.declare_entry ("Temporary output location", "",
                             Patterns::Anything(),
                             "On large clusters it can be advantageous to first write the "
                             "output to a temporary file on a local file system and later "
                             "move this file to a network file system. If this variable is "
                             "set to a non-empty string it will be interpreted as a "
                             "temporary storage location.");
#endif
        }
        prm.leave_subsection ();
      }
      prm.leave_subsection ();

      Particle::World<dim>::declare_parameters(prm);
    }


    template <int dim>
    void
    Particles<dim>::parse_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection("Postprocess");
      {
        prm.enter_subsection("Particles");
        {
          output_interval = prm.get_double ("Time between data output");
          if (this->convert_output_to_years())
            output_interval *= year_in_seconds;

          AssertThrow(this->get_parameters().run_postprocessors_on_nonlinear_iterations == false,
                      ExcMessage("Postprocessing nonlinear iterations in models with "
                                 "particles is currently not supported."));

#if DEAL_II_VERSION_GTE(9,0,0)
          output_formats   = Utilities::split_string_list(prm.get ("Data output format"));
          AssertThrow(Utilities::has_unique_entries(output_formats),
                      ExcMessage("The list of strings for the parameter "
                                 "'Particles/Data output format' contains entries more than once. "
                                 "This is not allowed. Please check your parameter file."));

          AssertThrow ((std::find (output_formats.begin(),
                                   output_formats.end(),
                                   "none") == output_formats.end())
                       ||
                       (output_formats.size() == 1),
                       ExcMessage ("If you specify 'none' for the parameter \"Data output format\", "
                                   "then this needs to be the only value given."));

          if (std::find (output_formats.begin(),
                         output_formats.end(),
                         "none") == output_formats.end())
            aspect::Utilities::create_directory (this->get_output_directory() + "particles/",
                                                 this->get_mpi_communicator(),
                                                 true);

          // Note: "ascii" is a legacy format used by ASPECT before particle output
          // in deal.II was implemented. It is nearly identical to the gnuplot format, thus
          // we simply replace "ascii" by "gnuplot" should it be selected.
          std::vector<std::string>::iterator output_format =  std::find (output_formats.begin(),
                                                                         output_formats.end(),
                                                                         "ascii");
          if (output_format != output_formats.end())
            *output_format = "gnuplot";

          group_files     = prm.get_integer("Number of grouped files");
          write_in_background_thread = prm.get_bool("Write in background thread");
          temporary_output_location = prm.get("Temporary output location");

          if (temporary_output_location != "")
            {
              // Check if a command-processor is available by calling system() with a
              // null pointer. System is guaranteed to return non-zero if it finds
              // a terminal and zero if there is none (like on the compute nodes of
              // some cluster architectures, e.g. IBM BlueGene/Q)
              AssertThrow(system((char *)0) != 0,
                          ExcMessage("Usage of a temporary storage location is only supported if "
                                     "there is a terminal available to move the files to their final location "
                                     "after writing. The system() command did not succeed in finding such a terminal."));
            }
#endif
        }
        prm.leave_subsection ();
      }
      prm.leave_subsection ();

      // Initialize the particle world
      if (SimulatorAccess<dim> *sim = dynamic_cast<SimulatorAccess<dim>*>(&world))
        sim->initialize_simulator (this->get_simulator());
      world.parse_parameters(prm);
      world.initialize();
    }
  }
}


// explicit instantiations
namespace aspect
{
  namespace Postprocess
  {
#if DEAL_II_VERSION_GTE(9,0,0)
    namespace internal
    {
#define INSTANTIATE(dim) \
  template class ParticleOutput<dim>;

      ASPECT_INSTANTIATE(INSTANTIATE)
    }
#endif

    ASPECT_REGISTER_POSTPROCESSOR(Particles,
                                  "particles",
                                  "A Postprocessor that creates particles that follow the "
                                  "velocity field of the simulation. The particles can be generated "
                                  "and propagated in various ways and they can carry a number of "
                                  "constant or time-varying properties. The postprocessor can write "
                                  "output positions and properties of all particles at chosen intervals, "
                                  "although this is not mandatory. It also allows other parts of the "
                                  "code to query the particles for information.")
  }
}
