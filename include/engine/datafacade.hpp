#ifndef OSRM_ENGINE_DATAFACADE_DATAFACADE_HPP
#define OSRM_ENGINE_DATAFACADE_DATAFACADE_HPP

#ifdef OSRM_EXTERNAL_MEMORY
#include "routing/compressed_datafacade.hpp"
#else
#include "engine/datafacade/contiguous_internalmem_datafacade.hpp"
#endif

namespace osrm
{
namespace engine
{

#ifdef OSRM_EXTERNAL_MEMORY
using DataFacadeBase = datafacade::BaseDataFacade;
#else
using DataFacadeBase = datafacade::ContiguousInternalMemoryDataFacadeBase;
#endif

template <typename AlgorithmT>
using DataFacade = datafacade::ContiguousInternalMemoryDataFacade<AlgorithmT>;
}
}

#endif
