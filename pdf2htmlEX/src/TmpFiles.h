#ifndef TMPFILES_H__
#define TMPFILES_H__

#include <string>
#include <set>
#include "Param.h"

namespace pdf2htmlEX {

#include <mutex>

class TmpFiles
{
public:
    explicit TmpFiles( const Param& param );
    ~TmpFiles();

    void add( const std::string& fn);
    double get_total_size() const; // This likely iterates files; if so, the lock should be inside or it's complex.
                                   // For now, assume internal operations on `tmp_files` set are protected.
private:
    void clean();

    const Param& param;
    std::set<std::string> tmp_files;
    mutable std::mutex mtx; // Mutable for get_total_size if it needs to lock
};

} // namespace pdf2htmlEX

#endif //TMPFILES_H__
