#include "lib/feature_min.h"
#include "lib/util.h"
#include "lib/database.h"
#include "lib/classifier.h"
#include "lib/tree_climber.h"
#include "lib/bitmap.h"
#include "lib/tx.h"
#include "lib/khpp.h"
#include "lib/glob.h"
#include "lib/flextree.h"
#include <functional>
#include <fstream>

using namespace emp;

using namespace std::literals;
using std::cerr;
using std::cout;

int classify_main(int argc, char *argv[]) {
    int co, num_threads(16), emit_kraken(1), emit_fastq(0), emit_all(0), chunk_size(1 << 20), per_set(32);
    std::FILE *ofp(stdout);
    if(argc < 4) {
        usage:
        std::fprintf(stderr, "Usage:\n%s <dbpath> <tax_path> <inr1.fq> [Optional: <inr2.fq>]\n"
                             "Flags:\n-o:\tRedirect output to path instead of stdout.\n"
                             "-c:\tSet chunk size. Default: %i\n"
                             "-a:\tEmit all records, not just classified.\n"
                             "-p:\tSet number of threads. Default: 16.\n"
                             "-k:\tEmit kraken-style output.\n"
                             "-K:\tDo not emit kraken-style output.\n"
                             "-f:\tEmit fastq-style output.\n"
                             "-K:\tDo not emit fastq-formatted output.\n"
                             "\nIf -f and -k are set, full kraken output will be contained in the fastq comment field."
                             "\n  Default: kraken-style only output.\n",
                 *argv, 1 << 14);
        std::exit(EXIT_FAILURE);
    }
    while((co = getopt(argc, argv, "c:p:o:S:afFkKh?")) >= 0) {
        switch(co) {
            case 'h': case '?': goto usage;
            case 'a': emit_all = 1; break;
            case 'c': chunk_size = atoi(optarg); break;
            case 'F': emit_fastq  = 0; break;
            case 'f': emit_fastq  = 1; break;
            case 'K': emit_kraken = 0; break;
            case 'k': emit_kraken = 1; break;
            case 'p': num_threads = atoi(optarg); break;
            case 'o': ofp = std::fopen(optarg, "w"); break;
            case 'S': per_set = atoi(optarg); break;
        }
    }
    LOG_ASSERT(ofp);
    switch(argc - optind) {
        default: goto usage;
        case 3:  LOG_DEBUG("Processing in single-end mode.\n"); break;
        case 4:  LOG_DEBUG("Processing in paired-end mode.\n"); break;
    }
    Database<khash_t(c)> db(argv[optind]);
    //reportDB<khash_t(c)>(&db, stderr);
    //for(auto &i: db._s) --i; // subtract by one since we'll re-subtract during construction.
    ClassifierGeneric<lex_score> c(db.db_, db.s_, db.k_, db.k_, num_threads,
                                   emit_all, emit_fastq, emit_kraken);
    khash_t(p) *taxmap(build_parent_map(argv[optind + 1]));
    // We can use optind + 3 for both single-end and paired-end mode since the argument at
    // index argc is null when argc - optind == 3.
    process_dataset(c, taxmap, argv[optind + 2], argv[optind + 3],
                    ofp, chunk_size, per_set);
    if(ofp != stdout) std::fclose(ofp);
    kh_destroy(p, taxmap);
    LOG_INFO("Successfully completed classify!\n");
    return EXIT_SUCCESS;
}

std::vector<std::string> get_paths(const char *path) {
    gzFile fp(gzopen(path, "rb"));
    char buf[1024], *line;
    std::vector<std::string> ret;
    while((line = gzgets(fp, buf, sizeof buf))) {
        ret.emplace_back(line);
        ret[ret.size() - 1].pop_back();
    }
    gzclose(fp);
    return ret;
}

int phase2_main(int argc, char *argv[]) {
    int c, mode(score_scheme::LEX), wsz(-1), num_threads(-1), k(31);
    std::size_t start_size(1<<16);
    std::string spacing, tax_path, seq2taxpath, paths_file;
    // TODO: update documentation for tax_path and seq2taxpath options.
    if(argc < 4) {
        usage:
        std::fprintf(stderr, "Usage: %s <flags> [tax_path if lex else <phase1map.path>] <out.path> <paths>\nFlags:\n"
                     "-k: Set k.\n"
                     "-p: Number of threads\n"
                     "-t: Build for taxonomic minimizing\n-f: Build for feature minimizing\n"
                     "-F: Load paths from file provided instead further arguments on the command-line.\n"
                     , *argv);
        std::exit(EXIT_FAILURE);
    }
    while((c = getopt(argc, argv, "w:M:S:p:k:T:F:tfHh?")) >= 0) {
        switch(c) {
            case 'h': case '?': goto usage;
            case 'k': k = atoi(optarg); break;
            case 'p': num_threads = atoi(optarg); break;
            case 'S': spacing = optarg; break;
            case 's': start_size = strtoull(optarg, nullptr, 10); break;
            case 't': mode = score_scheme::TAX_DEPTH; break;
            case 'f': mode = score_scheme::FEATURE_COUNT; break;
            case 'w': wsz = atoi(optarg); break;
            case 'T': tax_path = optarg; break;
            case 'M': seq2taxpath = optarg; break;
            case 'F': paths_file = optarg; break;
        }
    }
    if(wsz < 0 || wsz < k) LOG_EXIT("Window size must be set and >= k for phase2.\n");
    spvec_t sv(spacing.size() ? parse_spacing(spacing.data(), k): spvec_t(k - 1, 0));
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind + 2, argv + argc));
    LOG_DEBUG("Got paths\n");
    if(score_scheme::LEX == mode) {
        if(seq2taxpath.empty()) LOG_EXIT("seq2taxpath required for lexicographic mode for final database generation.");
        Spacer sp(k, wsz, sv);
        Database<khash_t(c)>  phase2_map(sp);
#if 0
        std::size_t hash_size(use_hll ? estimate_cardinality<lex_score>(inpaths, k, k, sp.s_, nullptr, num_threads, 24): 1 << 16);
#else
        // Force using hll so that we can use __sync_bool_compare_and_swap to parallelize.
        std::size_t hash_size(estimate_cardinality<lex_score>(inpaths, k, k, sp.s_, nullptr, num_threads, 24));
        LOG_DEBUG("Estimated cardinality: %zu\n", hash_size);
#endif
        LOG_DEBUG("Parent map bulding from %s\n", argv[optind]);
        khash_t(p) *taxmap(build_parent_map(argv[optind]));
        phase2_map.db_ = lca_map<lex_score>(inpaths, taxmap, seq2taxpath.data(), sp, num_threads, hash_size);
        phase2_map.write(argv[optind + 1]);
        kh_destroy(p, taxmap);
        return EXIT_SUCCESS;
    }
    Database<khash_t(64)> phase1_map{Database<khash_t(64)>(argv[optind])};
    Spacer sp(k, wsz, phase1_map.s_);
    Database<khash_t(c)>  phase2_map{phase1_map};
    khash_t(p) *taxmap(tax_path.empty() ? nullptr: build_parent_map(tax_path.data()));
    phase2_map.db_ = minimized_map<hash_score>(inpaths, phase1_map.db_, sp, num_threads, start_size);
    // Write minimized map
    phase2_map.write(argv[optind + 1]);
    if(taxmap) kh_destroy(p, taxmap);
    return EXIT_SUCCESS;
}


int hll_main(int argc, char *argv[]) {
    int c, wsz(-1), k(31), num_threads(-1), sketch_size(24);
    std::string spacing, paths_file;
    if(argc < 2) {
        usage: LOG_EXIT("Usage: %s <opts> <paths>\nFlags:\n"
                        "-k:\tkmer length (Default: 31. Max: 31)\n"
                        "-w:\twindow size (Default: -1)  Must be -1 (ignored) or >= kmer length.\n"
                        "-s:\tspacing (default: none). format: <value>x<times>,<value>x<times>,...\n"
                        "   \tOmitting x<times> indicates 1 occurrence of spacing <value>\n"
                        "-S:\tsketch size (default: 24). (Allocates 2 << [param] bytes of memory per HyperLogLog.\n"
                        "-p:\tnumber of threads.\n"
                        "-F:\tPath to file which contains one path per line\n"
                        , argv[0]);
    }
    while((c = getopt(argc, argv, "w:s:S:p:k:tfh?")) >= 0) {
        switch(c) {
            case 'h': case '?': goto usage;
            case 'k': k = atoi(optarg); break;
            case 'p': num_threads = atoi(optarg); break;
            case 's': spacing = optarg; break;
            case 'S': sketch_size = atoi(optarg); break;
            case 'w': wsz = atoi(optarg); break;
            case 'F': paths_file = optarg; break;
        }
    }
    if(wsz < k) wsz = k;
    std::vector<std::string> inpaths(paths_file.empty() ? get_paths(paths_file.data())
                                                        : std::vector<std::string>(argv + optind, argv + argc));
    spvec_t sv(spacing.empty() ? spvec_t(k - 1, 0): parse_spacing(spacing.data(), k));
    const std::size_t est(estimate_cardinality<lex_score>(inpaths, k, wsz, sv, nullptr, num_threads, sketch_size));
    std::fprintf(stderr, "Estimated number of unique exact matches: %zu\n", est);
    return EXIT_SUCCESS;
}

int phase1_main(int argc, char *argv[]) {
    int c, taxmap_preparsed(0), use_hll(0), mode(score_scheme::LEX), wsz(-1), k(31), num_threads(-1), sketch_size(24);
    std::string spacing;

    if(argc < 5) {
        usage:
        std::fprintf(stderr, "Usage: %s <flags> <seq2tax.path> <taxmap.path> <out.path> <paths>\nFlags:\n"
                     "-k: Set k.\n"
                     "-p: Number of threads.\n-S: add a spacer of the format "
                     "<int>,<int>,<int>, (...), where each integer is the number of spaces"
                     "between successive bases included in the seed. There must be precisely k - 1"
                     "elements in this list. Use this option multiple times to specify multiple seeds.\n"
                     "-s: add a spacer of the format <int>x<int>,<int>x<int>,"
                     "..., where the first integer corresponds to the space "
                     "between bases repeated the second integer number of times.\n"
                     "-S: Set HyperLogLog sketch size. For very large cardinalities, this may need to be increased for accuracy.\n"
                     "-t: Build for taxonomic minimizing.\n-f: Build for feature minimizing.\n"
                     "-H: Estimate rather than count kmers exactly before building map.\n"
                     "-T: Path to taxonomy map to load, if you've preparsed it. Not really worth it, building from scratch is fast.\n"
                     "-d: Write out in database format version 1.\n"
                     , *argv);
        std::exit(EXIT_FAILURE);
    }
    if("lca"s == argv[0])
        std::fprintf(stderr, "[W:%s] lca subcommand has been renamed phase1. "
                             "This has been deprecated and will be removed.\n", __func__);
    while((c = getopt(argc, argv, "s:S:p:k:tfTHh?")) >= 0) {
        switch(c) {
            case 'h': case '?': goto usage;
            case 'k': k = atoi(optarg); break;
            case 'p': num_threads = atoi(optarg); break;
            case 's': spacing = optarg; break;
            case 'S': sketch_size = atoi(optarg); break;
            case 'T': taxmap_preparsed = 1; break;
            case 'H': use_hll = 1; break;
            case 't': mode = score_scheme::TAX_DEPTH; break;
            case 'f': mode = score_scheme::FEATURE_COUNT; break;
            //case 'w': wsz = atoi(optarg); break;
        }
    }
    if(wsz < 0) wsz = k;
    khash_t(p) *taxmap(taxmap_preparsed ? khash_load<khash_t(p)>(argv[optind + 1])
                                        : build_parent_map(argv[optind + 1]));
    spvec_t sv(parse_spacing(spacing.data(), k));
    Spacer sp(k, wsz, sv);
    std::vector<std::string> inpaths(argv + optind + 3, argv + argc);
    std::size_t hash_size(use_hll ? estimate_cardinality<lex_score>(inpaths, k, k, sv, nullptr, num_threads, sketch_size): 1 << 16);
    if(use_hll) std::fprintf(stderr, "Estimated number of elements: %zu\n", hash_size);

    if(mode == score_scheme::LEX)
        LOG_EXIT("No phase1 required for lexicographic. Use phase2 instead.\n");
    auto mapbuilder(mode == score_scheme::TAX_DEPTH ? taxdepth_map<lex_score>
                                                    : ftct_map<lex_score>);

    Database<khash_t(64)> db(sp, 1, mapbuilder(inpaths, taxmap, argv[optind], sp, num_threads, hash_size));
    for(auto &i: db.s_) {
        LOG_DEBUG("Decrementing value %i to %i\n", i, i - 1);
        --i;
    }
    db.write(argv[optind + 2]);

    kh_destroy(p, taxmap);
    return EXIT_SUCCESS;
}

int metatree_usage(char *arg) {
    std::fprintf(stderr, "Usage: %s <db.path> <taxmap> <nameidmap> <out_taxmap> <out_taxkey>\n"
                         "\n"
                         "-F: Parse file paths from file instead of further arguments at command-line.\n"
                         "-d: Do not perform inversion (assume it's already been done.)\n"
                         "-f: Store binary dumps in folder <arg>.\n"
                 , arg);
    std::exit(EXIT_FAILURE);
    return EXIT_FAILURE;
}

template struct kh::khpp_t<std::vector<std::uint64_t> *, std::uint64_t, ptr_wang_hash_struct<std::vector<std::uint64_t> *>>;
using pkh_t = kh::khpp_t<std::vector<std::uint64_t> *, std::uint64_t, ptr_wang_hash_struct<std::vector<std::uint64_t> *>>;



int metatree_main(int argc, char *argv[]) {
    if(argc < 5) metatree_usage(*argv);
    int c, dry_run(0), num_threads(-1);
    std::string paths_file, folder, spacing;
    while((c = getopt(argc, argv, "p:w:k:s:f:F:h?d")) >= 0) {
        switch(c) {
            case '?': case 'h': return metatree_usage(*argv);
            case 'f': folder = optarg; break;
            case 'F': paths_file = optarg; break;
            case 'd': dry_run = 1; break;
            case 'p': num_threads = atoi(optarg); break;
        }
    }
    khash_t(name) *name_hash(build_name_hash(argv[optind + 2]));
    LOG_DEBUG("Parsed name hash.\n");
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind + 5, argv + argc));
#if !NDEBUG
    cerr << "Processing " << inpaths.size() << " inpaths:\n";
    for(const auto &str: inpaths) cerr << str << '\n';
#endif
    FlexMap fm(5);
#ifdef TAX_CHECK
    khash_t(p) *full_taxmap(build_parent_map(argv[optind + 1]));
    khash_t(p) *taxmap(tree::pruned_taxmap(inpaths, full_taxmap, name_hash));
    {
       auto kraken_tax(build_kraken_tax(argv[optind + 1]));
       {
           decltype(kraken_tax) pruned_kraken_tax;
           for(khiter_t ki(0); ki != kh_end(taxmap); ++ki) {
               if(kh_exist(taxmap, ki)) {
                   pruned_kraken_tax[kh_key(taxmap, ki)] = kraken_tax.at(kh_key(taxmap, ki));
               }
           }
           kraken_tax = std::move(pruned_kraken_tax);
       }
       std::vector<tax_t> nsv;
       {
           std::set<tax_t> nodeset;
           for(const auto &pair: kraken_tax) {
               nodeset.insert(pair.first);
               nodeset.insert(pair.second);
           }
           if(nodeset.find(0) != nodeset.end()) nodeset.erase(0);
           nsv = std::move(std::vector<tax_t>(nodeset.begin(), nodeset.end()));
       }
       for(auto i(nsv.cbegin()), e(nsv.cend()); i != e; ++i) {
           for(auto j(i + 1); j != e; ++j) {
               assert(lca(kraken_tax, *i, *j) == lca(taxmap, *i, *j));
           }
           if(((i - nsv.cbegin()) & 0xF) == 0) LOG_INFO("Processed %zd of %zu\n", i - nsv.cbegin(), nsv.size());
       }
    }
#else
    khash_t(p) *full_taxmap(build_parent_map(argv[optind + 1]));
    khash_t(p) *taxmap(tree::pruned_taxmap(inpaths, full_taxmap, name_hash));
#endif
    kh_destroy(p, full_taxmap);
    if(inpaths.empty()) LOG_EXIT("Need input files from command line or file. See usage.\n");

// Core
    std::vector<tax_t> taxes;
    {
        std::unordered_set<tax_t> taxset;
        for(khiter_t ki(0); ki != kh_end(taxmap); ++ki)
            if(kh_exist(taxmap, ki))
                taxset.insert(kh_key(taxmap, ki));
        taxes = std::vector<tax_t>(taxset.begin(), taxset.end());
    }
    std::unordered_map<tax_t, ClassLevel> taxclassmap;
    {
        std::ifstream ifs(argv[optind + 1]);
        std::string buffer;
        tax_t t;
        if(!ifs.good()) throw "a party";
        for(std::string line; std::getline(ifs, line);) {
            t = atoi(line.data());
            if(kh_get(p, taxmap, t) == kh_end(taxmap)) continue;
            taxclassmap.emplace(t, get_linelvl(line.data(), buffer, classlvl_map));
        }
    }
    std::sort(taxes.begin(), taxes.end(), [&tcm=taxclassmap](const tax_t a, const tax_t b) {
        auto ma(tcm.find(a)), mb(tcm.find(b));
        if(ma == tcm.end()) throw std::runtime_error("Missing taxid from tcm for a.");
        if(mb == tcm.end()) throw std::runtime_error("Missing taxid from tcm for b.");
        return (ma->second == mb->second) ? a < b: ma->second > mb->second;
    });
// Cleanup
    destroy_name_hash(name_hash);
    kh_destroy(p, taxmap);
    return EXIT_SUCCESS;
}

int hist_main(int argc, char *argv[]) {
    Database<khash_t(c)> db(argv[1]);
    khash_t(c) *map(db.db_);
    std::FILE *ofp(stdout);
    count::Counter<std::uint32_t> counter;
    if(argc > 2) ofp = std::fopen(argv[2], "w");
    for(khiter_t ki(0); ki != kh_end(map); ++ki) if(kh_exist(map, ki)) counter.add(kh_val(map, ki));
    auto &cmap(counter.get_map());
    using elcount = std::pair<tax_t, std::uint32_t>;
    std::vector<elcount> structs;
    for(auto& i: cmap) structs.emplace_back(i.first, i.second);
    std::sort(std::begin(structs), std::end(structs), [] (elcount &a, elcount &b) {
        return a.second < b.second;
    });
    std::fputs("Name\tCount\n", ofp);
    for(auto &i: structs) std::fprintf(ofp, "%u\t%u\n", i.first, i.second);
    if(ofp != stdout) std::fclose(ofp);
    return EXIT_SUCCESS;
}

static std::vector<std::pair<std::string, int (*)(int, char **)>> mains {
    {"phase1", phase1_main},
    {"p1",     phase1_main},
    {"phase2", phase2_main},
    {"p2",     phase2_main},
    {"lca", phase1_main},
    {"hll", hll_main},
    {"hist", hist_main},
    {"metatree", metatree_main},
    {"classify", classify_main}
};
int main(int argc, char *argv[]) {

    if(argc > 1) for(auto &i: mains) if(i.first == argv[1]) return i.second(argc - 1, argv + 1);
    std::fprintf(stderr, "No valid subcommand provided. Options: phase1, phase2, classify, hll, metatree\n");
    return EXIT_FAILURE;
}
