/**************************************************************************
 * Copyright Tom van Dijk
 *************************************************************************/

#include <aigmaker.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>

using namespace sylvan;


AIGmaker::AIGmaker(HoaData *data, SymGame *game) : data(data), game(game)
{
    a = aiger_init();
    lit = 2;
    uap_to_lit = new int[game->uap_count];
    state_to_lit = new int[game->statebits];
    caps = new char*[game->cap_count];

    // Set which APs are controllable in the bitset controllable
    pg::bitset controllable(data->noAPs);
    for (int i=0; i<data->noCntAPs; i++) {
        controllable[data->cntAPs[i]] = 1;
    }

    cap_bdds = new MTBDD[game->cap_count];
    for (int i=0; i<game->cap_count; i++) {
        cap_bdds[i] = mtbdd_false;
        mtbdd_protect(&cap_bdds[i]);
    }

    state_bdds = new MTBDD[game->statebits];
    for (int i=0; i<game->statebits; i++) {
        state_bdds[i] = mtbdd_false;
        mtbdd_protect(&state_bdds[i]);
    }

    int uap_idx = 0;
    int cap_idx = 0;
    for (int i=0; i<data->noAPs; i++) {
        if (!controllable[i]) {
            uap_to_lit[uap_idx] = lit;
            aiger_add_input(a, lit, data->aps[i]);
            uap_idx++;
            lit += 2;
        } else {
            caps[cap_idx++] = data->aps[i];
        }
    }

    for (int i=0; i<game->statebits; i++) {
        state_to_lit[i] = lit;
        lit += 2;
    }

    {
        MTBDD _vars = game->s_vars;
        for (int i=0; i<game->statebits; i++) {
            uint32_t bddvar = mtbdd_set_first(_vars);
            _vars = mtbdd_set_next(_vars);
            var_to_lit[bddvar] = state_to_lit[i];
            // std::cerr << "BDD variable " << bddvar << " is state " << i << " literal " << state_to_lit[i] << std::endl;
        }
        _vars = game->uap_vars;
        for (int i=0; i<game->uap_count; i++) {
            uint32_t bddvar = mtbdd_set_first(_vars);
            _vars = mtbdd_set_next(_vars);
            var_to_lit[bddvar] = uap_to_lit[i];
            // std::cerr << "BDD variable " << bddvar << " is uAP " << i << " literal " << uap_to_lit[i] << std::endl;
        }
    }

    {
        // compute bdds for the controllable APs
        for (int i=0; i<game->cap_count; i++) {
            MTBDD cap = sylvan_ithvar(mtbdd_set_first(game->cap_vars)+i);
            mtbdd_protect(&cap);
            // keep just s and u... get rid of other cap variables
            this->cap_bdds[i] = sylvan_and_exists(game->strategies, cap, game->cap_vars);
            mtbdd_unprotect(&cap);
        }
    }

    {
        // compute state bdds
        MTBDD su_vars = mtbdd_set_addall(game->p_vars, game->cap_vars);
        mtbdd_protect(&su_vars);

        // su_vars is priority and cap, which is to be removed...
        // full is: s > u > ns
        MTBDD full = mtbdd_and_exists(game->strategies, game->trans, su_vars);
        mtbdd_protect(&full);

        MTBDD ns = mtbdd_false;
        mtbdd_protect(&ns);

        for (int i=0; i<game->statebits; i++) {
            ns = sylvan_ithvar(mtbdd_set_first(game->ns_vars) + i);
            // don't care about the other next state variables... keep just s and u
            this->state_bdds[i] = sylvan_and_exists(full, ns, game->ns_vars);
        }

        mtbdd_unprotect(&ns);
        mtbdd_unprotect(&full);
        mtbdd_unprotect(&su_vars);
    }
}

AIGmaker::~AIGmaker()
{
    delete[] uap_to_lit;
    delete[] state_to_lit;
    delete[] caps;

    for (int i=0; i<game->cap_count; i++) mtbdd_unprotect(&cap_bdds[i]);
    for (int i=0; i<game->statebits; i++) mtbdd_unprotect(&state_bdds[i]);

    delete[] cap_bdds;
    delete[] state_bdds;
}

int
AIGmaker::makeand(int rhs0, int rhs1)
{
    if (rhs1 < rhs0) {
        int tmp = rhs0;
        rhs0 = rhs1;
        rhs1 = tmp;
    }

    if (rhs0 == 0) return 0;
    if (rhs0 == 1) return rhs1;

    uint64_t cache_key = rhs1;
    cache_key <<= 32;
    cache_key |= rhs0;
    auto c = cache.find(cache_key);
    if (1 and c != cache.end()) {
        return c->second;
    } else {
        aiger_add_and(a, lit, rhs0, rhs1);
        cache[cache_key] = lit;
        lit += 2;
        return lit-2;
    }
}

void
AIGmaker::simplify_and(std::deque<int> &gates)
{
    // for each pair of gates in gates, check the cache
    for (auto first = gates.begin(); first != gates.end(); ++first) {
        for (auto second = first + 1; second != gates.end(); ++second) {
            int left = *first;
            int right = *second;
            if (left > right) std::swap(left, right);
            uint64_t cache_key = right;
            cache_key <<= 32;
            cache_key |= left;
            auto c = cache.find(cache_key);
            if (c != cache.end()) {
                //gates.erase(std::remove_if(gates.begin(), gates.end(), [=](int x){return x==left or x==right;}),
                //        gates.end());
                gates.erase(second);
                gates.erase(first);
                gates.push_back(c->second);
                simplify_and(gates);
                return;
            }
        }
    }
}

void
AIGmaker::simplify_or(std::deque<int> &gates)
{
    // for each pair of gates in gates, check the cache
    for (auto first = gates.begin(); first != gates.end(); ++first) {
        for (auto second = first + 1; second != gates.end(); ++second) {
            int left = aiger_not(*first);
            int right = aiger_not(*second);
            if (left > right) std::swap(left, right);
            uint64_t cache_key = right;
            cache_key <<= 32;
            cache_key |= left;
            auto c = cache.find(cache_key);
            if (c != cache.end()) {
                gates.erase(second);
                gates.erase(first);
                gates.push_back(aiger_not(c->second));
                simplify_or(gates);
                return;
            }
        }
    }
}

int
AIGmaker::bdd_to_aig_isop(MTBDD bdd)
{
    if (verbose) {
        std::cerr << "running isop for BDD with " << mtbdd_nodecount(bdd) << " nodes." << std::endl;
    }
    MTBDD bddres;
    ZDD isop = zdd_isop(bdd, bdd, &bddres);
    zdd_protect(&isop);
    // no need to reference the result...
    assert(bdd == bddres);
    assert(bdd == zdd_cover_to_bdd(isop));
    if (verbose) {
        std::cerr << "isop has " << (long)zdd_pathcount(isop) << " terms and " << zdd_nodecount(&isop, 1) << " nodes." << std::endl;
    }

    int res = bdd_to_aig_cover(isop);
    zdd_unprotect(&isop);
    return res;
}

int
AIGmaker::bdd_to_aig_cover_sop(ZDD cover)
{
    if (cover == zdd_true) return aiger_true;
    if (cover == zdd_false) return aiger_false;

    // a product could consist of all variables, and a -1 to denote
    //  the end of the product
    int product[game->statebits+game->uap_count+1] = { 0 };

    // a queue that stores all products, which will need to be summed
    std::deque<int> products;

    ZDD res = zdd_cover_enum_first(cover, product);
    while (res != zdd_false) {
        //  containing subproducts in the form of gates
        std::deque<int> gates;

        for (int i=0; product[i] != -1; i++) {
            int the_lit = var_to_lit[product[i]/2];
            if (product[i]&1) the_lit = aiger_not(the_lit);
            gates.push_back(the_lit);
        }

        // simplify_and(gates);

        // while we still have subproducts we need to AND together
        while (!gates.empty()) {
            int last = gates.front();
            gates.pop_front();
            if (!gates.empty()) {
                int last2 = gates.front();
                gates.pop_front();
                int new_gate = makeand(last, last2);
                gates.push_back(new_gate);
            } else {
                products.push_back(last);
            }
        }
        res = zdd_cover_enum_next(cover, product); // go to the next product
    }

    // products queue should now be full of complete products that need to be summed

    // simplify_or(products);

    while (!products.empty()) {
        int product1 = products.front();
        products.pop_front();
        if (!products.empty()) {
            int product2 = products.front();
            products.pop_front();
            int summed_product = aiger_not(makeand(aiger_not(product1), aiger_not(product2)));
            products.push_back(summed_product);
        } else { // product1 is the final sum of all products
            return product1;
        }
    }

    return aiger_false; // should be unreachable, tbh.
}

int
AIGmaker::bdd_to_aig_cover(ZDD cover)
{
    if (cover == zdd_true) return aiger_true;
    if (cover == zdd_false) return aiger_false;

    auto it = mapping.find(cover);
    if (it != mapping.end()) {
        return it->second;
    }

    int the_var = zdd_getvar(cover);
    int the_lit = var_to_lit[the_var/2];
    if (the_var & 1) the_lit = aiger_not(the_lit);

    ZDD low = zdd_getlow(cover);
    ZDD high = zdd_gethigh(cover);

    int res = the_lit;

    if (high != zdd_true) {
        auto x = bdd_to_aig_cover(high);
        res = makeand(res, x);
    }

    if (low != zdd_false) {
        auto x = bdd_to_aig_cover(low);
        res = aiger_not(makeand(aiger_not(res), aiger_not(x)));
    }

    mapping[cover] = res;
    return res;
}

int
AIGmaker::bdd_to_aig(MTBDD bdd)
{
    if (bdd == mtbdd_true) return aiger_true;
    if (bdd == mtbdd_false) return aiger_false;
 
    bool comp = false;
    if (bdd & sylvan_complement) {
        bdd ^= sylvan_complement;
        comp = true;
    }

    auto it = mapping.find(bdd);
    if (it != mapping.end()) {
        return comp ? aiger_not(it->second) : it->second;
    }

    int the_lit = var_to_lit[mtbdd_getvar(bdd)];

    MTBDD low = mtbdd_getlow(bdd);
    MTBDD high = mtbdd_gethigh(bdd);

    int res;

    if (low == mtbdd_false) {
        // only high (value 1)
        if (high == mtbdd_true) {
            // actually this is the end, just the lit
            res = the_lit;
        } else {
            // AND(the_lit, ...)
            int rhs0 = the_lit;
            int rhs1 = bdd_to_aig(high);
            res = makeand(rhs0, rhs1);
        }
    } else if (high == mtbdd_false) {
        // only low (value 0)
        if (low == mtbdd_true) {
            // actually this is the end, just the lit, negated
            res = aiger_not(the_lit);
        } else {
            // AND(not the_lit, ...)
            int rhs0 = aiger_not(the_lit);
            int rhs1 = bdd_to_aig(low);
            res = makeand(rhs0, rhs1);
        }
    } else {
        // OR(low, high) == ~AND(~AND(the_lit, ...), ~AND(~the_lit, ...))
        int lowres = bdd_to_aig(low);
        int highres = bdd_to_aig(high);
        int rhs0 = aiger_not(makeand(aiger_not(the_lit), lowres));
        int rhs1 = aiger_not(makeand(the_lit, highres));
        res = aiger_not(makeand(rhs0, rhs1));
    }
        
    mapping[bdd] = res;

    return comp ? aiger_not(res) : res;
}

void
AIGmaker::processCAP(int i, MTBDD bdd)
{
    int res = isop ? bdd_to_aig_isop(bdd) : bdd_to_aig(bdd);
    aiger_add_output(a, res, caps[i]); // simple, really
}

void
AIGmaker::processState(int i, MTBDD bdd)
{
    int res = isop ? bdd_to_aig_isop(bdd) : bdd_to_aig(bdd);
    aiger_add_latch(a, state_to_lit[i], res, "");
}

void
AIGmaker::process()
{
    // if ISOP, first convert all cap bdds etc to covers
    if (isop) {
        ZDD* cap_zdds = new ZDD[game->cap_count];
        for (int i=0; i<game->cap_count; i++) {
            cap_zdds[i] = zdd_false;
            zdd_protect(&cap_zdds[i]);

            MTBDD bddres;
            cap_zdds[i] = zdd_isop(cap_bdds[i], cap_bdds[i], &bddres);
            assert(bddres == cap_bdds[i]);
            if (verbose) {
                std::cerr << "isop has " << (long)zdd_pathcount(cap_zdds[i]) << " terms and " << zdd_nodecount(&cap_zdds[i], 1) << " nodes." << std::endl;
            }
        }

        ZDD* state_zdds = new ZDD[game->statebits];
        for (int i=0; i<game->statebits; i++) {
            state_zdds[i] = zdd_false;
            zdd_protect(&state_zdds[i]);

            MTBDD bddres;
            state_zdds[i] = zdd_isop(state_bdds[i], state_bdds[i], &bddres);
            assert(bddres == state_bdds[i]);
            if (verbose) {
                std::cerr << "isop has " << (long)zdd_pathcount(state_zdds[i]) << " terms and " << zdd_nodecount(&state_zdds[i], 1) << " nodes." << std::endl;
            }
        }

        for (int i=0; i<game->cap_count; i++) {
            int res = bdd_to_aig_cover(cap_zdds[i]);
            aiger_add_output(a, res, caps[i]); // simple, really
        }
        for (int i=0; i<game->statebits; i++) {
            int res = bdd_to_aig_cover(state_zdds[i]);
            aiger_add_latch(a, state_to_lit[i], res, "");
        }
    } else {
        for (int i=0; i<game->cap_count; i++) {
            processCAP(i, cap_bdds[i]);
        }
        for (int i=0; i<game->statebits; i++) {
            processState(i, state_bdds[i]);
        }
    }
}

void
AIGmaker::write(FILE* out)
{
    aiger_write_to_file(a, aiger_ascii_mode, out);
}

void
AIGmaker::writeBinary(FILE* out)
{
    aiger_write_to_file(a, aiger_binary_mode, out);
}

// commands taken from 'alias compress2rs' from 'abc.rc' file
const std::vector<std::string> AIGmaker::compressCommands ({
    "balance -l",
    "resub -K 6 -l",
    "rewrite -l",
    "resub -K 6 -N 2",
    "refactor -l",
    "resub -K 8 -l",
    "balance -l",
    "resub -K 8 -N 2 -l",
    "rewrite -l",
    "resub -K 10 -l",
    "rewrite -z -l",
    "resub -K 10 -N 2 -l",
    "balance -l",
    "resub -K 12 -l",
    "refactor -z -l",
    "resub -K 12 -N 2 -l",
    "balance -l",
    "rewrite -z -l",
    "balance -l"
});

// commands taken from 'alias compress2' from 'abc.rc' file
/*
const std::vector<std::string> AIGmaker::compressCommands ({
    "balance -l",
    "rewrite -l",
    "refactor -l",
    "balance -l",
    "rewrite -l",
    "rewrite -z -l",
    "balance -l",
    "refactor -z -l",
    "rewrite -z -l",
    "balance -l"
});
*/

void
AIGmaker::compress()
{
    Abc_Start();
    Abc_Frame_t* pAbc = Abc_FrameGetGlobalFrame();

    writeToAbc(pAbc);

    // compress until convergence
    int new_num_nodes = getAbcNetworkSize(pAbc);
    int old_num_nodes = new_num_nodes + 1;
    while (new_num_nodes > 0 && new_num_nodes < old_num_nodes) {
        executeCompressCommands(pAbc);
        old_num_nodes = new_num_nodes;
        new_num_nodes = getAbcNetworkSize(pAbc);
        // std::cerr << "nodes after compress run: " << new_num_nodes << std::endl;
        if ((old_num_nodes-new_num_nodes)<old_num_nodes/20) break; // 5% improvement or better pls
    }

    readFromAbc(pAbc);

    Abc_Stop();
}

void AIGmaker::executeAbcCommand(Abc_Frame_t* pAbc, const std::string command) const {
    if (Cmd_CommandExecute( pAbc, command.c_str())) {
        throw std::runtime_error("Cannot execute ABC command: " + command);
    }
}

void AIGmaker::executeCompressCommands(Abc_Frame_t* pAbc) const {
    for (const auto& command : compressCommands) {
        // std::cerr << "executing " << command << std::endl;
        executeAbcCommand(pAbc, command);
        // std::cerr << "nodes after compress run: " << getAbcNetworkSize(pAbc) << std::endl;
    }
}

int AIGmaker::getAbcNetworkSize(Abc_Frame_t* pAbc) const {
    Abc_Ntk_t* pNtk = Abc_FrameReadNtk(pAbc);
    return Abc_NtkNodeNum(pNtk);
}

int AIGmaker::getTmpFile(char* tmp_filename) const {
    boost::filesystem::path tmpfile_template_path = boost::filesystem::temp_directory_path() / "knor.XXXXXX";
    std::string tmpfile_template = tmpfile_template_path.string();
    std::strcpy(tmp_filename, tmpfile_template.c_str());
    int fd = mkstemp(tmp_filename);
    if (fd == -1) {
        throw std::runtime_error("Could not create temporary file: " + std::string(tmp_filename));
    }
    return fd;
}

void AIGmaker::writeToAbc(Abc_Frame_t* pAbc) const {
    char tmp_filename[256];
    int fd = getTmpFile(tmp_filename);

    // write AIGER out to be read by ABC
    FILE* file = fdopen(fd, "w");
    if (file == nullptr) {
        throw std::runtime_error("Could not open temporary file: " + std::string(tmp_filename));
    }
    int write_result = aiger_write_to_file(a, aiger_binary_mode, file);
    fclose(file);
    if (write_result == 0) {
        throw std::runtime_error("Could not write AIGER circuit to file: " + std::string(tmp_filename));
    }

    std::stringstream read_command;
    read_command << "read_aiger " << tmp_filename;
    executeAbcCommand(pAbc, read_command.str());

    std::remove(tmp_filename);
}

void AIGmaker::readFromAbc(Abc_Frame_t* pAbc) {
    char tmp_filename[256];
    int fd = getTmpFile(tmp_filename);

    std::stringstream write_command;
    write_command << "write_aiger -s " << tmp_filename;
    executeAbcCommand(pAbc, write_command.str());

    // read AIGER back, delete comments added by ABC
    FILE* file = fdopen(fd, "r");
    if (file == nullptr) {
        throw std::runtime_error("Could not open temporary file: " + std::string(tmp_filename));
    }
    // read_aiger
    aiger_reset(a);
    a = aiger_init();
    const char* read_result = aiger_read_from_file(a, file);
    fclose(file);
    std::remove(tmp_filename);
    if (read_result != nullptr) {
        throw std::runtime_error("Could not read AIGER circuit from file: " + std::string(tmp_filename) + ": " + std::string(read_result));
    }
    aiger_delete_comments(a);
}

