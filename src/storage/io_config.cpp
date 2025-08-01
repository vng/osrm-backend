#include "storage/io_config.hpp"

#include "util/log.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

namespace osrm
{
namespace storage
{
bool IOConfig::IsValid() const
{
    bool success = true;
    for (auto const & fileName : required_input_files)
    {
        using namespace boost::filesystem;
        if (!is_regular_file(path{base_path.string() + fileName.string()}))
        {
            util::Log(logWARNING) << "Missing/Broken File: " << base_path.string()
                                  << fileName.string();
            success = false;
        }
    }
    return success;
}
}
}
