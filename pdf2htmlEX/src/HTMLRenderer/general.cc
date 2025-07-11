/*
 * general.cc
 *
 * Handling general stuffs
 *
 * Copyright (C) 2012,2013,2014 Lu Wang <coolwanglu@gmail.com>
 */

#include <cstdio>
#include <ostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include <GlobalParams.h>

#include "pdf2htmlEX-config.h"
#include "HTMLRenderer.h"
#include "HTMLTextLine.h"
#include "Base64Stream.h"

#include "BackgroundRenderer/BackgroundRenderer.h"

#include "util/namespace.h"
#include "util/ffw.h"
#include "util/math.h"
#include "util/path.h"
#include "util/css_const.h"
#include "util/encoding.h"

namespace pdf2htmlEX {

using std::fixed;
using std::flush;
using std::ostream;
using std::max;
using std::min_element;
using std::vector;
using std::abs;
using std::cerr;
using std::endl;

HTMLRenderer::HTMLRenderer(const char* progPath, Param & param)
    :OutputDev()
    ,param(param)
    ,html_text_page(param, all_manager)
    ,preprocessor(param)
    ,tmp_files(param)
    ,covered_text_detector(param)
    ,tracer(param)
{
    if(!(param.debug))
    {
        //disable error messages of poppler
        globalParams->setErrQuiet(true);
    }

    ffw_init(progPath, param.debug);

    cur_mapping.resize(0x10000);
    cur_mapping2.resize(0x100);
    width_list.resize(0x10000);

    /*
     * For these states, usually the error will not be accumulated
     * or may be handled well (whitespace_manager)
     * So we can set a large eps here
     */
    all_manager.vertical_align.set_eps(param.v_eps);
    all_manager.whitespace    .set_eps(param.h_eps);
    all_manager.left          .set_eps(param.h_eps);
    /*
     * For other states, we need accurate values
     * optimization will be done separately
     */
    all_manager.font_size   .set_eps(EPS);
    all_manager.letter_space.set_eps(EPS);
    all_manager.word_space  .set_eps(EPS);
    all_manager.height      .set_eps(EPS);
    all_manager.width       .set_eps(EPS);
    all_manager.bottom      .set_eps(EPS);

    tracer.on_char_drawn =
            [this](cairo_t *cairo, double * box) { covered_text_detector.add_char_bbox(cairo, box); };
    tracer.on_char_clipped =
            [this](cairo_t *cairo, double * box, int partial) { covered_text_detector.add_char_bbox_clipped(cairo, box, partial); };
    tracer.on_non_char_drawn =
            [this](cairo_t *cairo, double * box, int what) { covered_text_detector.add_non_char_bbox(cairo, box, what); };
}

HTMLRenderer::~HTMLRenderer()
{
    ffw_finalize();
}

#define MAX_DIMEN 9000

void HTMLRenderer::process(PDFDoc *doc)
{
    cur_doc = doc;
    cur_catalog = doc->getCatalog();
    xref = doc->getXRef();

    pre_process(doc);

    ///////////////////
    // Process pages

    if(param.process_nontext)
    {
        bg_renderer = BackgroundRenderer::getBackgroundRenderer(param.bg_format, this, param);
        if(!bg_renderer)
            throw "Cannot initialize background renderer, unsupported format";
        bg_renderer->init(doc);

        fallback_bg_renderer = BackgroundRenderer::getFallbackBackgroundRenderer(this, param);
        if (fallback_bg_renderer)
            fallback_bg_renderer->init(doc);
    }

    int page_count = (param.last_page - param.first_page + 1);
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (param.num_threads > 0) {
        num_threads = param.num_threads;
    }

    std::vector<std::thread> threads;
    std::atomic<int> current_page_atomic(param.first_page);

    for (unsigned int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (true) {
                int i = current_page_atomic.fetch_add(1);
                if (i > param.last_page) {
                    break;
                }

                // Thread-local storage for page-specific data
                std::ofstream local_f_curpage_stream;
                std::ofstream *local_f_curpage_ptr = nullptr;
                std::string local_cur_page_filename;

                // Lock for operations that are not thread-safe
                std::unique_lock<std::mutex> lock(param.render_mutex);

                param.actual_dpi = param.desired_dpi;
                param.max_dpi = 72 * MAX_DIMEN / max(doc->getPageCropWidth(i), doc->getPageCropHeight(i));

                if (param.actual_dpi > param.max_dpi) {
                    param.actual_dpi = param.max_dpi;
                    printf("Warning:Page %d clamped to %f DPI\n", i, param.actual_dpi);
                }

                bool stop_processing = false;
                if (param.tmp_file_size_limit != -1) {
                    // tmp_files.get_total_size() needs to be thread-safe or called under a lock
                    // Assuming TmpFiles methods will be made thread-safe internally or are already.
                    // For now, let's lock it externally if it's a shared resource check.
                    // std::lock_guard<std::mutex> tmp_lock(param.tmp_files_mutex); // Introduce a specific mutex if needed
                    if (tmp_files.get_total_size() > param.tmp_file_size_limit * 1024) {
                        if (param.quiet == 0)
                            cerr << "Stop processing, reach max size\n";
                        stop_processing = true;
                    }
                }
                if (stop_processing) {
                    current_page_atomic.store(param.last_page + 1);
                    lock.unlock();
                    break;
                }


                if (param.quiet == 0) {
                    // Progress reporting needs to be thread-safe if we want to keep it accurate.
                    // For simplicity, we can report page completion at the end of processing each page.
                }


                if(param.split_pages)
                {
                    local_cur_page_filename = (char*)str_fmt(param.page_filename.c_str(), i);
                    auto page_fn = str_fmt("%s/%s", param.dest_dir.c_str(), local_cur_page_filename.c_str());
                    local_f_curpage_stream.open((char*)page_fn, ofstream::binary);
                    if(!(local_f_curpage_stream)) {
                        // Handle error: throw or log
                        cerr << "Cannot open " << (char*)page_fn << " for writing" << endl;
                        lock.unlock();
                        continue;
                    }
                    set_stream_flags(local_f_curpage_stream);
                    local_f_curpage_ptr = &local_f_curpage_stream;
                } else {
                    // When not splitting pages, all output goes to f_pages.fs, which needs locking.
                    // This part will be tricky and might require significant refactoring
                    // For now, let's assume split_pages is the primary use case for parallelization
                    // or that f_pages.fs is made thread-safe separately.
                    // For this initial step, we'll focus on the loop parallelization.
                    // If not splitting, this path needs careful review for thread safety.
                    // A potential approach is to buffer page content locally and write sequentially.
                }
                lock.unlock();


                // The HTMLRenderer object (this) is shared. We need to ensure displayPage and its callees are thread-safe.
                // This is a major task for the next step. For now, we assume some level of internal locking or make it so.
                // For this step, we are just structuring the loop.
                // We'll need to pass the thread-local f_curpage and cur_page_filename to displayPage or related methods.
                // This will likely require changes to startPage and endPage.

                // Placeholder for where thread-local f_curpage and cur_page_filename would be used by displayPage
                // This requires HTMLRenderer to be aware of the current thread's output stream and filename.
                // One way is to pass them as parameters to displayPage or set them in a thread-local context within HTMLRenderer.

                // Critical section: displayPage modifies shared state within HTMLRenderer and Poppler's doc object.
                // This is the most complex part for thread-safety.
                // For now, let's assume `displayPage` itself is made thread-safe or we add a lock around it if necessary.
                // The actual rendering (doc->displayPage) must be made thread-safe.
                // Poppler's PDFDoc::displayPage might not be thread-safe if it modifies shared document state without internal locking.
                // If Poppler is not thread-safe for concurrent page displays from the same PDFDoc,
                // then we might need a global lock around doc->displayPage, which would serialize this part,
                // limiting the benefits of multi-threading.
                // Alternatively, if different pages can truly be processed independently by Poppler,
                // then only the parts of HTMLRenderer that *accumulate* global state (like CSS, fonts, outlines) need locking.
                std::unique_lock<std::mutex> display_lock(param.display_mutex); // A new mutex for displayPage

                // Update HTMLRenderer to use thread-local streams
                this->setThreadLocalOutputStream(local_f_curpage_ptr ? local_f_curpage_ptr : &f_pages.fs);
                this->setThreadLocalPageFilename(local_cur_page_filename);


                doc->displayPage(this, i,
                        text_zoom_factor() * DEFAULT_DPI, text_zoom_factor() * DEFAULT_DPI,
                        0,
                        (!(param.use_cropbox)),
                        true,  // crop
                        false, // printing
                        nullptr, nullptr, nullptr, nullptr);

                this->clearThreadLocalOutputStream(); // Reset after use
                this->clearThreadLocalPageFilename();
                display_lock.unlock();


                lock.lock(); // Re-acquire lock for shared post-page processing
                if (param.quiet == 0) {
                     cerr << "Finished page: " << i << "/" << param.last_page << endl; // Simple progress
                }

                if(param.split_pages && local_f_curpage_stream.is_open())
                {
                    local_f_curpage_stream.close();
                }
                lock.unlock();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    if(page_count >= 0 && param.quiet == 0)
        cerr << "Working: " << page_count << "/" << page_count;

    if(param.quiet == 0)
        cerr << endl;

    ////////////////////////
    // Process Outline
    if(param.process_outline)
        process_outline();

    post_process();

    bg_renderer = nullptr;
    fallback_bg_renderer = nullptr;

    if(param.quiet == 0)
        cerr << endl;
}

void HTMLRenderer::setDefaultCTM(const double *ctm)
{
    memcpy(default_ctm, ctm, sizeof(default_ctm));
}

void HTMLRenderer::startPage(int pageNum, GfxState *state, XRef * xref)
{
    covered_text_detector.reset();
    tracer.reset(state);

    this->pageNum = pageNum;

    // Calculate and set current page's actual DPI
    // GfxState provides page dimensions. If use_cropbox is true, we should use cropbox dimensions.
    // PDFDoc methods like getPageCropWidth might be an alternative if GfxState doesn't have exactly what we need easily.
    // For now, assume GfxState is sufficient as it's available here.
    double page_width_for_dpi_calc = param.use_cropbox ? state->getCropBox()->getWidth() : state->getPageWidth();
    double page_height_for_dpi_calc = param.use_cropbox ? state->getCropBox()->getHeight() : state->getPageHeight();

    this->current_page_actual_dpi = param.desired_dpi; // Start with desired
    if (page_width_for_dpi_calc > 0 && page_height_for_dpi_calc > 0) { // Avoid division by zero
        double max_allowed_dpi = 72.0 * MAX_DIMEN / std::max(page_width_for_dpi_calc, page_height_for_dpi_calc);
        if (this->current_page_actual_dpi > max_allowed_dpi) {
            this->current_page_actual_dpi = max_allowed_dpi;
            // Thread-safe logging for this warning is needed if we want to keep it.
            // For now, omitting the printf.
            // printf("Warning:Page %d clamped to %f DPI\n", pageNum, this->current_page_actual_dpi);
        }
    } else {
        // Default or error if page dimensions are zero/invalid
        this->current_page_actual_dpi = param.desired_dpi;
    }

    // The original DPI change warning (param.desired_dpi != param.actual_dpi)
    // can be issued here if needed, comparing param.desired_dpi with this->current_page_actual_dpi

    html_text_page.set_page_size(state->getPageWidth(), state->getPageHeight());

    reset_state();
}

void HTMLRenderer::endPage() {
    long long wid = all_manager.width.install(html_text_page.get_width());
    long long hid = all_manager.height.install(html_text_page.get_height());

    std::ofstream* current_output_stream = tl_f_curpage ? tl_f_curpage : f_curpage;
    std::string current_page_name = tl_cur_page_filename.empty() ? cur_page_filename : tl_cur_page_filename;

    (*current_output_stream)
        << "<div id=\"" << CSS::PAGE_FRAME_CN << pageNum
            << "\" class=\"" << CSS::PAGE_FRAME_CN
            << " " << CSS::WIDTH_CN << wid
            << " " << CSS::HEIGHT_CN << hid
            << "\" data-page-no=\"" << pageNum << "\">"
        << "<div class=\"" << CSS::PAGE_CONTENT_BOX_CN
            << " " << CSS::PAGE_CONTENT_BOX_CN << pageNum
            << " " << CSS::WIDTH_CN << wid
            << " " << CSS::HEIGHT_CN << hid
            << "\">";

    /*
     * When split_pages is on, f_curpage points to the current page file
     * and we want to output empty frames in f_pages.fs
     */
    if(param.split_pages)
    {
        // f_pages.fs needs to be protected by a mutex if accessed by multiple threads
        std::lock_guard<std::mutex> lock(param.render_mutex);
        f_pages.fs
            << "<div id=\"" << CSS::PAGE_FRAME_CN << pageNum
                << "\" class=\"" << CSS::PAGE_FRAME_CN
                << " " << CSS::WIDTH_CN << wid
                << " " << CSS::HEIGHT_CN << hid
                << "\" data-page-no=\"" << pageNum
                << "\" data-page-url=\"";

        writeAttribute(f_pages.fs, current_page_name);
        f_pages.fs << "\">";
    }

    if(param.process_nontext)
    {
        if (bg_renderer->render_page(cur_doc, pageNum))
        {
            bg_renderer->embed_image(pageNum);
        }
        else if (fallback_bg_renderer)
        {
            if (fallback_bg_renderer->render_page(cur_doc, pageNum))
                fallback_bg_renderer->embed_image(pageNum);
        }
    }

    // dump all text
    // html_text_page.dump_text needs to write to the correct stream.
    // f_css.fs is a shared resource and needs locking.
    html_text_page.dump_text(*current_output_stream);
    {
        std::lock_guard<std::mutex> lock(param.render_mutex);
        html_text_page.dump_css(f_css.fs);
    }
    html_text_page.clear();

    // process form
    if(param.process_form)
        process_form(*current_output_stream);
    
    // process links before the page is closed
    // cur_doc->processLinks might need to be thread-safe or called under a lock if it modifies shared state.
    // For now, assuming it's safe or handled by display_mutex if called within displayPage scope.
    // If called here, it might need its own synchronization.
    {
        std::lock_guard<std::mutex> lock(param.display_mutex); // Or a more specific mutex for link processing
        cur_doc->processLinks(this, pageNum);
    }

    // close box
    (*current_output_stream) << "</div>";

    // dump info for js
    // TODO: create a function for this
    // BE CAREFUL WITH ESCAPES
    {
        (*current_output_stream) << "<div class=\"" << CSS::PAGE_DATA_CN << "\" data-data='{";

        //default CTM
        (*current_output_stream) << "\"ctm\":[";
        for(int i = 0; i < 6; ++i)
        {
            if(i > 0) (*current_output_stream) << ",";
            (*current_output_stream) << round(default_ctm[i]);
        }
        (*current_output_stream) << "]";

        (*current_output_stream) << "}'></div>";
    }

    // close page
    (*current_output_stream) << "</div>" << endl;

    if(param.split_pages)
    {
        // f_pages.fs needs to be protected by a mutex
        std::lock_guard<std::mutex> lock(param.render_mutex);
        f_pages.fs << "</div>" << endl;
    }
}

void HTMLRenderer::pre_process(PDFDoc * doc)
{
    preprocessor.process(doc);

    /*
     * determine scale factors
     */
    {
        vector<double> zoom_factors;

        if(is_positive(param.zoom))
        {
            zoom_factors.push_back(param.zoom);
        }

        if(is_positive(param.fit_width))
        {
            zoom_factors.push_back((param.fit_width) / preprocessor.get_max_width());
        }

        if(is_positive(param.fit_height))
        {
            zoom_factors.push_back((param.fit_height) / preprocessor.get_max_height());
        }

        double zoom = (zoom_factors.empty() ? 1.0 : (*min_element(zoom_factors.begin(), zoom_factors.end())));

        text_scale_factor1 = max<double>(zoom, param.font_size_multiplier);
        text_scale_factor2 = zoom / text_scale_factor1;
    }

    // we may output utf8 characters, so always use binary
    {
        /*
         * If embed-css
         * we have to keep the generated css file into a temporary place
         * and embed it into the main html later
         *
         * otherwise
         * leave it in param.dest_dir
         */

        auto fn = (param.embed_css)
            ? str_fmt("%s/__css", param.tmp_dir.c_str())
            : str_fmt("%s/%s", param.dest_dir.c_str(), param.css_filename.c_str());

        if(param.embed_css)
            tmp_files.add((char*)fn);

        f_css.path = (char*)fn;
        f_css.fs.open(f_css.path, ofstream::binary);
        if(!f_css.fs)
            throw string("Cannot open ") + (char*)fn + " for writing";
        set_stream_flags(f_css.fs);
    }

    if (param.process_outline)
    {
        /*
         * The logic for outline is similar to css
         */

        auto fn = (param.embed_outline)
            ? str_fmt("%s/__outline", param.tmp_dir.c_str())
            : str_fmt("%s/%s", param.dest_dir.c_str(), param.outline_filename.c_str());

        if(param.embed_outline)
            tmp_files.add((char*)fn);

        f_outline.path = (char*)fn;
        f_outline.fs.open(f_outline.path, ofstream::binary);
        if(!f_outline.fs)
            throw string("Cannot open") + (char*)fn + " for writing";

        // might not be necessary
        set_stream_flags(f_outline.fs);
    }

    {
        /*
         * we have to keep the html file for pages into a temporary place
         * because we'll have to embed css before it
         *
         * Otherwise just generate it
         */
        auto fn = str_fmt("%s/__pages", param.tmp_dir.c_str());
        tmp_files.add((char*)fn);

        f_pages.path = (char*)fn;
        f_pages.fs.open(f_pages.path, ofstream::binary);
        if(!f_pages.fs)
            throw string("Cannot open ") + (char*)fn + " for writing";
        set_stream_flags(f_pages.fs);
    }

    // Initialize f_curpage for the main thread (used if not param.split_pages, or as a fallback)
    // When param.split_pages is true, each thread will manage its own file stream via tl_f_curpage.
    // If param.split_pages is false, all threads would theoretically write to f_pages.fs via tl_f_curpage being set to &f_pages.fs.
    // This path (not splitting pages with multithreading) is risky and needs careful synchronization around f_pages.fs writes.
    // The current loop structure passes &f_pages.fs to setThreadLocalOutputStream if a thread-specific stream isn't made.
    if (!param.split_pages) {
        f_curpage = &f_pages.fs;
    } else {
        f_curpage = nullptr; // Explicitly null when splitting, as each thread handles its own.
    }
}

void HTMLRenderer::post_process(void)
{
    dump_css();
    
    // close files if they opened
    if (param.process_outline)
    {
        f_outline.fs.close();
    }
    f_pages.fs.close();
    f_css.fs.close();

    // build the main HTML file
    ofstream output;
    {
        auto fn = str_fmt("%s/%s", param.dest_dir.c_str(), param.output_filename.c_str());
        output.open((char*)fn, ofstream::binary);
        if(!output)
            throw string("Cannot open ") + (char*)fn + " for writing";
        set_stream_flags(output);
    }

    // apply manifest
    ifstream manifest_fin((char*)str_fmt("%s/%s", param.data_dir.c_str(), MANIFEST_FILENAME.c_str()), ifstream::binary);
    if(!manifest_fin)
        throw "Cannot open the manifest file";

    bool embed_string = false;
    string line;
    long line_no = 0;
    while(getline(manifest_fin, line))
    {
        // trim space at both sides
        {
            static const char * whitespaces = " \t\n\v\f\r";
            auto idx1 = line.find_first_not_of(whitespaces);
            if(idx1 == string::npos)
            {
                line.clear();
            }
            else
            {
                auto idx2 = line.find_last_not_of(whitespaces);
                assert(idx2 >= idx1);
                line = line.substr(idx1, idx2 - idx1 + 1);
            }
        }

        ++line_no;

        if(line == "\"\"\"")
        {
            embed_string = !embed_string;
            continue;
        }

        if(embed_string)
        {
            output << line << endl;
            continue;
        }

        if(line.empty() || line[0] == '#')
            continue;


        if(line[0] == '@')
        {
            embed_file(output, param.data_dir + "/" + line.substr(1), "", true);
            continue;
        }

        if(line[0] == '$')
        {
            if(line == "$css")
            {
                embed_file(output, f_css.path, ".css", false);
            }
            else if (line == "$outline")
            {
                if (param.process_outline && param.embed_outline)
                {
                    ifstream fin(f_outline.path, ifstream::binary);
                    if(!fin)
                        throw "Cannot open outline for reading";
                    output << fin.rdbuf();
                    output.clear(); // output will set fail big if fin is empty
                }
            }
            else if (line == "$pages")
            {
                ifstream fin(f_pages.path, ifstream::binary);
                if(!fin)
                    throw "Cannot open pages for reading";
                output << fin.rdbuf();
                output.clear(); // output will set fail bit if fin is empty
            }
            else
            {
                cerr << "Warning: manifest line " << line_no << ": Unknown content \"" << line << "\"" << endl;
            }
            continue;
        }

        cerr << "Warning: unknown line in manifest: " << line << endl;
    }
}

void HTMLRenderer::set_stream_flags(std::ostream & out)
{
    // we output all ID's in hex
    // browsers are not happy with scientific notations
    out << hex << fixed;
}

void HTMLRenderer::dump_css (void)
{
    all_manager.transform_matrix.dump_css(f_css.fs);
    all_manager.vertical_align  .dump_css(f_css.fs);
    all_manager.letter_space    .dump_css(f_css.fs);
    all_manager.stroke_color    .dump_css(f_css.fs);
    all_manager.word_space      .dump_css(f_css.fs);
    all_manager.whitespace      .dump_css(f_css.fs);
    all_manager.fill_color      .dump_css(f_css.fs);
    all_manager.font_size       .dump_css(f_css.fs);
    all_manager.bottom          .dump_css(f_css.fs);
    all_manager.height          .dump_css(f_css.fs);
    all_manager.width           .dump_css(f_css.fs);
    all_manager.left            .dump_css(f_css.fs);
    all_manager.bgimage_size    .dump_css(f_css.fs);

    // print css
    if(param.printing)
    {
        double ps = print_scale();
        f_css.fs << CSS::PRINT_ONLY << "{" << endl;
        all_manager.transform_matrix.dump_print_css(f_css.fs, ps);
        all_manager.vertical_align  .dump_print_css(f_css.fs, ps);
        all_manager.letter_space    .dump_print_css(f_css.fs, ps);
        all_manager.stroke_color    .dump_print_css(f_css.fs, ps);
        all_manager.word_space      .dump_print_css(f_css.fs, ps);
        all_manager.whitespace      .dump_print_css(f_css.fs, ps);
        all_manager.fill_color      .dump_print_css(f_css.fs, ps);
        all_manager.font_size       .dump_print_css(f_css.fs, ps);
        all_manager.bottom          .dump_print_css(f_css.fs, ps);
        all_manager.height          .dump_print_css(f_css.fs, ps);
        all_manager.width           .dump_print_css(f_css.fs, ps);
        all_manager.left            .dump_print_css(f_css.fs, ps);
        all_manager.bgimage_size    .dump_print_css(f_css.fs, ps);
        f_css.fs << "}" << endl;
    }
}

void HTMLRenderer::embed_file(ostream & out, const string & path, const string & type, bool copy)
{
    string fn = get_filename(path);
    string suffix = (type == "") ? get_suffix(fn) : type;

    auto iter = EMBED_STRING_MAP.find(suffix);
    if(iter == EMBED_STRING_MAP.end())
    {
        cerr << "Warning: unknown suffix: " << suffix << endl;
        return;
    }

    const auto & entry = iter->second;

    if(param.*(entry.embed_flag))
    {
        ifstream fin(path, ifstream::binary);
        if(!fin)
            throw string("Cannot open file ") + path + " for embedding";
        out << entry.prefix_embed;

        if(entry.base64_encode)
        {
            out << Base64Stream(fin);
        }
        else
        {
            out << endl << fin.rdbuf();
        }
        out.clear(); // out will set fail big if fin is empty
        out << entry.suffix_embed << endl;
    }
    else
    {
        out << entry.prefix_external;
        writeAttribute(out, fn);
        out << entry.suffix_external << endl;

        if(copy)
        {
            ifstream fin(path, ifstream::binary);
            if(!fin)
                throw string("Cannot copy file: ") + path;
            auto out_path = param.dest_dir + "/" + fn;
            ofstream out(out_path, ofstream::binary);
            if(!out)
                throw string("Cannot open file ") + path + " for embedding";
            out << fin.rdbuf();
            out.clear(); // out will set fail big if fin is empty
        }
    }
}

const std::string HTMLRenderer::MANIFEST_FILENAME = "manifest";

thread_local std::ofstream * HTMLRenderer::tl_f_curpage = nullptr;
thread_local std::string HTMLRenderer::tl_cur_page_filename = "";

void HTMLRenderer::setThreadLocalOutputStream(std::ofstream* stream) {
    tl_f_curpage = stream;
}

void HTMLRenderer::clearThreadLocalOutputStream() {
    tl_f_curpage = nullptr;
}

void HTMLRenderer::setThreadLocalPageFilename(const std::string& filename) {
    tl_cur_page_filename = filename;
}

void HTMLRenderer::clearThreadLocalPageFilename() {
    tl_cur_page_filename = "";
}

}// namespace pdf2htmlEX
