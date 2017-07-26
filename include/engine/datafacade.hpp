#ifndef OSRM_ENGINE_DATAFACADE_DATAFACADE_HPP
#define OSRM_ENGINE_DATAFACADE_DATAFACADE_HPP

#ifdef OSRM_EXTERNAL_MEMORY
#include "engine/datafacade/datafacade_base.hpp"
#else
#include "engine/datafacade/contiguous_internalmem_datafacade.hpp"
#endif

namespace osrm::engine
{

#ifdef OSRM_EXTERNAL_MEMORY
using DataFacadeBase = datafacade::BaseDataFacade;
#else
using DataFacadeBase = datafacade::ContiguousInternalMemoryDataFacadeBase;
#endif

#ifdef OSRM_EXTERNAL_MEMORY
namespace datafacade
{
template <typename AlgorithmT> class ContiguousInternalMemoryDataFacade;
}
#endif

template <typename AlgorithmT>
using DataFacade = datafacade::ContiguousInternalMemoryDataFacade<AlgorithmT>;
} // namespace osrm::engine

#endif
