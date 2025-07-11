/*
 * TmpFiles.cc
 *
 * Collect and clean-up temporary files
 *
 * implemented by WangLu
 * split off by Filodej <philodej@gmail.com>
 */

#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "TmpFiles.h"
#include "Param.h"

#ifdef __MINGW32__
#include "util/mingw.h"
#endif

using namespace std;

namespace pdf2htmlEX {

TmpFiles::TmpFiles( const Param& param )
    : param( param )
{ }

TmpFiles::~TmpFiles()
{
    clean();
}

void TmpFiles::add( const string & fn)
{
    if(!param.clean_tmp)
        return;

    std::lock_guard<std::mutex> lock(mtx);
    if(tmp_files.insert(fn).second && param.debug)
        cerr << "Add new temporary file: " << fn << endl;
}

// Return the total size of the temporary files in bytes
double TmpFiles::get_total_size() const
{
    std::lock_guard<std::mutex> lock(mtx);
    double total_size = 0;
    struct stat st;
    for(auto & fn : tmp_files)
    {
        stat(fn.c_str(), &st);
        total_size += st.st_size;
    }

    return total_size;
}


void TmpFiles::clean()
{
    if(!param.clean_tmp)
        return;

    std::lock_guard<std::mutex> lock(mtx);
    // Make a copy of filenames to avoid issues if rmdir fails before all removes are done,
    // though the primary concern is concurrent access to the set.
    std::set<std::string> files_to_remove = tmp_files;
    tmp_files.clear(); // Clear original set under lock

    for(auto & fn : files_to_remove)
    {
        remove(fn.c_str());
        if(param.debug)
            cerr << "Remove temporary file: " << fn << endl;
    }

    // rmdir is not thread-safe if other threads might still be creating files in tmp_dir,
    // but clean() is called from destructor, implying single-thread access at that point.
    // If TmpFiles instances could be destructed concurrently, this rmdir might need more thought.
    // For now, assume clean() in destructor context is safe from concurrent TmpFiles operations.
    rmdir(param.tmp_dir.c_str());
    if(param.debug)
        cerr << "Remove temporary directory: " << param.tmp_dir << endl;
}

} // namespace pdf2htmlEX

