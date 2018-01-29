#include <float.h>
#include <math.h>
#include <stdio.h>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <queue>
#include <ctime>
#include <chrono>
#include <random>
#include <cstring>
#include <chrono>

#include "reductions.h"
#include "vw.h"

using namespace std;
using namespace LEARNER;

#define DEBUG false
#define D_COUT if(DEBUG) cerr

#define STATS false

struct node{
    uint32_t base_predictor; //id of the base predictor
    uint32_t label;

    node* parent; // pointer to the parent node
    node* temp;
    uint32_t next_to_expand;
    vector<node*> children; // pointers to the children nodes
    bool internal; // internal or leaf
    bool inverted;
    float t;
    float p; // prediction value
    bool operator < (const node& r) const { return p < r.p; }
};

struct oplt {
    vw* all;

    size_t k;
    size_t predictor_bits;
    size_t max_predictors;
    size_t base_predictor_count;
    size_t kary;

    node *tree_root;
    vector<node*> tree; // pointers to tree nodes
    unordered_map<uint32_t, node*> tree_leaves; // leaves map

    float inner_threshold;  // inner threshold
    bool positive_labels;   // print positive labels
    bool top_k_labels;   // print top-k labels
    bool greedy;
    uint32_t p_at_k;
    float precision;
    float predicted_number;
    v_array<float> precision_at_k;
    size_t prediction_count;
    uint32_t next_to_expand;

    // node expanding
    void(*copy)(oplt& p, uint32_t wv1, uint32_t wv2);
    node*(*add_new_label)(oplt& p, base_learner& base, example& ec, uint32_t new_label);
    default_random_engine rng;

    // stats
    uint32_t pass_count;
    uint32_t ec_count;
    uint32_t node_count;
    long n_visited_nodes;
    v_array<float> predictions;

    // save/load tree structure
    bool save_tree_structure;
    string save_tree_structure_file;

    chrono::time_point<chrono::steady_clock> learn_predict_start_time_point;
};


// debug helpers - to delete
//----------------------------------------------------------------------------------------------------------------------

void oplt_example_info(oplt& p, base_learner& base, example& ec){
    cerr << "TAG: " << (ec.tag.size() ? std::string(ec.tag.begin()) : "-") << " FEATURES COUNT: " << ec.num_features
         << " LABELS COUNT: " << ec.l.cs.costs.size() << endl;

    cerr << "BW: " << base.weights << " BI: " << base.increment
         << " WSS: " << p.all->weights.stride_shift() << " WM: " << p.all->weights.mask() << endl;

    for (features &fs : ec) {
        for (features::iterator_all &f : fs.values_indices_audit())
            cerr << "FEATURE: " << (f.index() & p.all->weights.mask()) << " VALUE: " << f.value() << endl;
    }
    for (auto &cl : ec.l.cs.costs) cerr << "LABEL: " << cl.class_index << endl;
}

void oplt_tree_info(oplt& p){
    cerr << "TREE SIZE: " << p.tree.size() << " TREE LEAVES: " << p.tree_leaves.size() << "\nTREE:\n";
    queue<node*> n_queue;
    n_queue.push(p.tree_root);

    size_t depth = 0;
    while(!n_queue.empty()) {
        size_t q_size = n_queue.size();
        cerr << "DEPTH " << depth << ": ";
        for(size_t i = 0; i < q_size; ++i){
            node *n = n_queue.front();
            n_queue.pop();

            if(n->parent) cerr << "[" << n->parent->base_predictor << "]";
            cerr << n->base_predictor;
            if(!n->internal) cerr << "(" << n->label << ")";
            cerr << " ";

            for(auto c : n->children) n_queue.push(c);
        }
        ++depth;
        cerr << endl;
    }
}


// helpers
//----------------------------------------------------------------------------------------------------------------------

inline float sigmoid(float in) { return 1.0f / (1.0f + exp(-in)); }

bool compare_node_ptr_func(const node* l, const node* r) { return (*l < *r); }

struct compare_node_ptr_functor{
    bool operator()(const node* l, const node* r) const { return (*l < *r); }
};

node* init_node(oplt& p) {
    node* n = new node();
    n->base_predictor = p.base_predictor_count++;
    n->children.reserve(0);
    n->internal = true;
    n->inverted = false;
    n->parent = nullptr;
    n->temp = nullptr;
    n->next_to_expand = 0;
    n->t = p.all->initial_t;
    //p.tree.push_back(n);
    if(STATS) ++p.node_count;

    D_COUT << "NODE INITED: T: " << n->t << endl;

    return n;
}

void node_set_parent(oplt& p, node *n, node *parent){
    n->parent = parent;
    p.tree.push_back(n);
    parent->children.push_back(n);
    parent->internal = true;
}

void node_set_label(oplt& p, node *n, uint32_t label){
    n->internal = false;
    n->label = label;
    p.tree_leaves[label] = n;
}

node* node_copy(oplt& p, node *n){
    node* c = init_node(p);
    p.copy(p, n->base_predictor, c->base_predictor);
    c->t = n->t;
    c->inverted = n->inverted;

    return c;
}

template<bool stride>
void copy_weights(oplt& p, uint32_t wv1, uint32_t wv2){
    parameters &weights = p.all->weights;
    uint64_t mask = weights.mask();

    if(stride){

        uint32_t stride_shift = weights.stride_shift();
        uint32_t mask_shift = p.predictor_bits + stride_shift;
        size_t stride_size = (1 << stride_shift) * sizeof(uint32_t);
        uint64_t wv_count = mask >> mask_shift;

        wv1 = wv1 << stride_shift;
        wv2 = wv2 << stride_shift;

        for (uint64_t i = 0; i <= wv_count; ++i) {
            uint64_t idx = (i << mask_shift); //& mask;
            memcpy(&weights[idx + wv2], &weights[idx + wv1], stride_size);
        }
    }
    else{

        uint64_t wv_count = mask >> p.predictor_bits;

        for (uint64_t i = 0; i <= wv_count; ++i) {
            uint64_t idx = (i << p.predictor_bits); //& mask;
            weights[idx + wv2] = weights[idx + wv1];
        }
    }

}

void init_tree(oplt& p){
    p.base_predictor_count = 0;
    p.tree_leaves = unordered_map<uint32_t, node*>();
    p.tree_root = init_node(p); // root node
    p.tree_root->temp = init_node(p); // first temp node
    p.tree.push_back(p.tree_root);
    p.next_to_expand = 0;
}


// save/load
//----------------------------------------------------------------------------------------------------------------------

void save_load_tree(oplt& p, io_buf& model_file, bool read, bool text){

    D_COUT << "SAVE/LOAD TREE\n";

    if (model_file.files.size() > 0) {
        bool resume = p.all->save_resume;
        stringstream msg;
        msg << ":" << resume << "\n";
        bin_text_read_write_fixed(model_file, (char*) &resume, sizeof(resume), "", read, msg, text);

        // read/write predictor_bits
        msg << "predictor_bits = " << p.predictor_bits;
        bin_text_read_write_fixed(model_file, (char*)&p.predictor_bits, sizeof(p.predictor_bits), "", read, msg, text);

        // read/write number of predictors
        msg << " base_predictor_count = " << p.base_predictor_count;
        bin_text_read_write_fixed(model_file, (char*)&p.base_predictor_count, sizeof(p.base_predictor_count), "", read, msg, text);

        // read/write nodes
        size_t n_size;
        if(!read) n_size = p.tree.size();
        bin_text_read_write_fixed(model_file, (char*)&n_size, sizeof(n_size), "", read, msg, text);
        msg << " tree_size = " << n_size;

        if(read){
            for(size_t i = 0; i < n_size - 1; ++i) { // root and temp are already in tree after init
                node *n = new node();
                p.tree.push_back(n);
            }
        }

        // read/write root and temp nodes
        uint32_t root_predictor;
        if(!read) root_predictor = p.tree_root->base_predictor;
        bin_text_read_write_fixed(model_file, (char*)&root_predictor, sizeof(root_predictor), "", read, msg, text);

        // read/write base predictor, label
        for(auto n : p.tree) {
            bin_text_read_write_fixed(model_file, (char *) &n->base_predictor, sizeof(n->base_predictor), "", read, msg, text);
            bin_text_read_write_fixed(model_file, (char *) &n->label, sizeof(n->label), "", read, msg, text);
            bin_text_read_write_fixed(model_file, (char *) &n->inverted, sizeof(n->inverted), "", read, msg, text);
            bin_text_read_write_fixed(model_file, (char *) &n->internal, sizeof(n->internal), "", read, msg, text);
        }

        // read/write parent and rebuild tree
        for(auto n : p.tree) {
            uint32_t parent_base_predictor, temp_base_predictor;

            if(!read){
                if(n->parent) parent_base_predictor = n->parent->base_predictor;
                else parent_base_predictor = -1;
                if(n->temp) temp_base_predictor = n->temp->base_predictor;
                else temp_base_predictor = -1;
            }

            bin_text_read_write_fixed(model_file, (char*)&parent_base_predictor, sizeof(parent_base_predictor), "", read, msg, text);
            bin_text_read_write_fixed(model_file, (char*)&temp_base_predictor, sizeof(temp_base_predictor), "", read, msg, text);

            if(read){
                if(n->base_predictor == root_predictor) p.tree_root = n;

                for (auto m : p.tree) {
                    if (m->base_predictor == parent_base_predictor) {
                        n->parent = m;
                        m->children.push_back(n);
                    }
                    if (m->base_predictor == temp_base_predictor) {
                        m->temp = n;
                    }
                }
            }
        }

        // recreate leafs index
        if(read) {
            for (auto n : p.tree)
                if (!n->internal) p.tree_leaves[n->label] = n;
        }

        if(resume){
            for(auto n : p.tree)
                bin_text_read_write_fixed(model_file, (char *) &n->t, sizeof(n->t), "", read, msg, text);
        }

        if(DEBUG) oplt_tree_info(p);
    }
}

vector<int> oplt_parse_line(string text, char d = ' '){
    vector<int> result;
    const char *str = text.c_str();

    do {
        const char *begin = str;
        while(*str != d && *str) ++str;
        result.push_back(stoi(string(begin, str)));
    } while (0 != *str++);

    return result;
}

// load tree structure
void load_tree_structure(oplt& p, string file_name){
    ifstream file;
    file.open(file_name);
    if(file.is_open()){
        string line;
        uint32_t line_count = 0;

        while(getline(file, line)) {
            ++line_count;
            if(!line.size() || line[0] == '#')
                continue;

            vector<int> nodes;

            try {
                nodes = oplt_parse_line(line);
            }
            catch(...){
                cerr << "Something is wrong with line " << line_count << " in " << file_name << "!\n";
                continue;
            };

            node* parent = nullptr;
            node* child = nullptr;
            for(auto n : p.tree){
                if(n->base_predictor == nodes[0]) parent = n;
                else if(n->base_predictor == nodes[1]) child = n;
            }
            if(!parent){
                parent = init_node(p);
                parent->base_predictor = nodes[0];
            }
            if(!child){
                child = init_node(p);
                child->base_predictor = nodes[1];
            }
            node_set_parent(p, child, parent);

            if(nodes.size() >= 3){
                node_set_label(p, child, nodes[2]);
            }
        }

        p.k = p.tree_leaves.size();
        p.tree_root->temp = nullptr;

        file.close();
        cerr << "Tree structure loaded from " << file_name << endl;
    }
}

// save tree structure
void save_tree_structure(oplt& p, string file_name){
    ofstream file;
    file.open(file_name);
    if(file.is_open()){
        for(auto n : p.tree){
            for(auto c : n->children){
                file << n->base_predictor << " " << c->base_predictor;
                if(!c->internal) file << " " << c->label;
                file << endl;
            }
        }

        file.close();
        cerr << "Tree structure saved to " << file_name << endl;
    }
}

// learn
//----------------------------------------------------------------------------------------------------------------------

inline void learn_node(oplt& p, node* n, base_learner& base, example& ec){
    D_COUT << "LEARN NODE: " << n->base_predictor << " LABEL: " << ec.l.simple.label << " WEIGHT: " << ec.weight << " NODE_T: " << n->t << endl;

    p.all->sd->t = n->t;
    n->t += ec.weight;

    if(n->inverted){
        ec.l.simple.label *= -1.0f;
        base.learn(ec, n->base_predictor);
        ec.l.simple.label *= -1.0f;
    }
    else base.learn(ec, n->base_predictor);
    ++p.n_visited_nodes;
}

inline float predict_node(oplt &p, node *n, base_learner& base, example& ec){
    D_COUT << "PREDICT NODE: " << n->base_predictor << endl;

    ec.l.simple = {FLT_MAX, 0.f, 0.f};
    base.predict(ec, n->base_predictor);

    if(n->inverted) ec.partial_prediction *= -1.0f;
    ++p.n_visited_nodes;

    return sigmoid(ec.partial_prediction);
}

node* expand_node(oplt& p, node* n, uint32_t new_label){

    D_COUT << "EXPAND NODE: BASE: " << n->base_predictor << " LABEL: " << n->label << " NEW LABEL: " << new_label << endl;

    if(!n->temp){
        cerr << "Node " << n->base_predictor << " doesn't have temp node and can't be expanded.";
        exit(0);
    }

    if(p.tree.size() >= p.max_predictors){
        cerr << "Max number of nodes reached, tree can't be expanded.";
        exit(0);
    }

    if(n->children.size() == 0){
        node* parent_label_node = node_copy(p, n->temp);
        parent_label_node->inverted = true;

        node_set_parent(p, parent_label_node, n);
        node_set_label(p, parent_label_node, n->label);

        parent_label_node->temp = node_copy(p, n->temp);
    }

    node *new_label_node;

    if(n->children.size() == p.kary - 1) {
        new_label_node = n->temp;
        n->temp = nullptr;
    }
    else
        new_label_node = node_copy(p, n->temp);

    node_set_parent(p, new_label_node, n);
    node_set_label(p, new_label_node, new_label);
    new_label_node->temp = init_node(p);
    n->children.shrink_to_fit();

    D_COUT << "LABELS: " << p.tree_leaves.size() << " / " << p.k << endl;
    D_COUT << "PREDICTORS: " << p.tree.size() << " / " << p.max_predictors << endl;
    if(DEBUG) oplt_tree_info(p);

    return new_label_node;
}

template<bool best_predicion, bool balanced, bool complete>
node* add_new_label(oplt& p, base_learner& base, example& ec, uint32_t new_label){
    D_COUT << "NEW LABEL: " << new_label << endl;

    // if first label
    if(p.tree_leaves.size() == 0){
        node_set_label(p, p.tree_root, new_label);
        return p.tree_root;
    }

    node* to_expand = p.tree_root;

    if(best_predicion) { // best prediction
        while (to_expand->children.size() >= p.kary) {
            node* best_child = to_expand->children[0];
            float best_P = 0;
            for(auto &child : to_expand->children){
                float P = predict_node(p, child, base, ec);
                if(P > best_P){
                    best_child = child;
                    best_P = P;
                }
            }
            to_expand = best_child;
        }
    }
    else if(balanced) { // balanced
        while (to_expand->children.size() >= p.kary) {
            to_expand = to_expand->children[to_expand->next_to_expand++ % to_expand->children.size()];
        }
    }
    else if(complete){
        to_expand = p.tree[p.next_to_expand];
        if(to_expand->children.size() >= p.kary)
            ++p.next_to_expand;
        to_expand = p.tree[p.next_to_expand];
    }
    else{ // random policy
        uniform_int_distribution <uint32_t> dist(0, p.kary - 1);
        while (to_expand->children.size() >= p.kary) {
            to_expand = to_expand->children[dist(p.rng)];
        }
    }

    return expand_node(p, to_expand, new_label);
}

void learn(oplt& p, base_learner& base, example& ec){

    if(!p.ec_count) p.learn_predict_start_time_point = chrono::steady_clock::now();

    D_COUT << "LEARN EXAMPLE: " << p.ec_count << " PASS: " << p.pass_count << "\n";
    if(DEBUG) oplt_example_info(p, base, ec);

    COST_SENSITIVE::label ec_labels = ec.l.cs;
    double t = p.all->sd->t;
    double weighted_holdout_examples = p.all->sd->weighted_holdout_examples;
    p.all->sd->weighted_holdout_examples = 0;

    unordered_set<node*> n_positive; // positive nodes
    unordered_set<node*> n_negative; // negative nodes

    if (ec_labels.costs.size() > 0) {
        if(p.pass_count == 0) {
            for (auto &cl : ec_labels.costs) {
                if (p.tree_leaves.find(cl.class_index) == p.tree_leaves.end()) {
                    p.add_new_label(p, base, ec, cl.class_index);
                }
            }
        }

        for (auto &cl : ec_labels.costs) {
            node *n = p.tree_leaves[cl.class_index];
            n_positive.insert(n);
            if(n->temp) n_negative.insert(n->temp);

            while (n->parent) {
                n = n->parent;
                n_positive.insert(n);
            }
        }

        queue<node*> n_queue; // nodes queue
        n_queue.push(p.tree_root); // push root

        while(!n_queue.empty()) {
            node* n = n_queue.front(); // current node index
            n_queue.pop();

            if (n->internal) {
                for(auto child : n->children) {
                    if (n_positive.find(child) != n_positive.end()) n_queue.push(child);
                    else n_negative.insert(child);
                }
                if(n->temp) n_negative.insert(n->temp);
            }
        }
    }
    else
        n_negative.insert(p.tree_root);

    // learn positive and negative
    ec.l.simple = {1.f, 0.f, 0.f};
    for (auto &n : n_positive) learn_node(p, n, base, ec);

    ec.l.simple.label = -1.f;
    for (auto &n : n_negative) learn_node(p, n, base, ec);

    ec.l.cs = ec_labels;
    p.all->sd->t = t;
    p.all->sd->weighted_holdout_examples = weighted_holdout_examples;
    ec.pred.multiclass = 0;

    ++p.ec_count;

    if(STATS){
        if(p.ec_count % 1000 == 0){
            cerr << "example_count = " << p.ec_count << endl;
            cerr << "node_count = " << p.node_count << " / " << p.max_predictors << endl;
            cerr << "tree_size = " << p.tree.size() << endl;
            cerr << "tree_leaves_count = " << p.tree_leaves.size() << endl;
            uint32_t temp_count = 0, internal_count = 0, internal_temp = 0;
            for(auto n : p.tree){
                if(n->temp) ++temp_count;
                if(n->internal){
                    ++internal_count;
                    if(n->temp) ++internal_temp;
                }
            }
            cerr << "temp_node_count = " << temp_count << endl;
            cerr << "internal_node_count = " << internal_count << endl;
            cerr << "internal_temp_node_count = " << internal_temp << endl;
            cerr << "------------------------------------------------------------\n";
        }
    }

    D_COUT << "----------------------------------------------------------------------------------------------------\n";
}


// predict
//----------------------------------------------------------------------------------------------------------------------

template<bool use_threshold, bool greedy>
void predict(oplt& p, base_learner& base, example& ec){

    if(!p.ec_count) p.learn_predict_start_time_point = chrono::steady_clock::now();

    D_COUT << "PREDICT EXAMPLE\n";
    if(DEBUG) oplt_example_info(p, base, ec);
    ++p.prediction_count;

    COST_SENSITIVE::label ec_labels = ec.l.cs;

    // threshold prediction
    if (use_threshold) {
        vector<node*> positive_labels;
        queue<node*> n_queue;

        p.tree_root->p = 1.0f;
        n_queue.push(p.tree_root);

        while (!n_queue.empty()) {
            node *n = n_queue.front(); // current node
            n_queue.pop();

            float cp = n->p * predict_node(p, n, base, ec);

            if (cp > p.inner_threshold) {
                if (n->internal) {
                    for (auto child : n->children) {
                        child->p = cp;
                        n_queue.push(child);
                    }
                } else {
                    n->p = cp;
                    positive_labels.push_back(n);
                }
            }
        }

        sort(positive_labels.rbegin(), positive_labels.rend(), compare_node_ptr_func);

        if (p.p_at_k > 0 && ec_labels.costs.size() > 0) {
            for (size_t i = 0; i < p.p_at_k && i < positive_labels.size(); ++i) {
                p.predicted_number += 1.0f;
                for (auto &cl : ec_labels.costs) {
                    if (positive_labels[i]->label == cl.class_index) {
                        p.precision += 1.0f;
                        break;
                    }
                }
            }
        }
    }

    else if (greedy) {
        node* current = p.tree_root;
        current->p = predict_node(p, current, base, ec);

        while(current->internal){
            node* best = current->children[0];

            for (auto child : current->children) {
                child->p = current->p * predict_node(p, child, base, ec);
                if(best->p < child->p) best = child;
            }

            current = best;
        }

        vector<uint32_t> true_labels;
        for (auto &cl : ec_labels.costs) true_labels.push_back(cl.class_index);

        if (find(true_labels.begin(), true_labels.end(), current->label) != true_labels.end())
            p.precision_at_k[0] += 1.0f;
    }

    // top-k predictions
    else{
        vector<node*> best_labels, found_leaves;
        priority_queue<node*, vector<node*>, compare_node_ptr_functor> n_queue;

        p.tree_root->p = 1.0f;
        n_queue.push(p.tree_root);

        p.predictions.erase();

        while (!n_queue.empty()) {
            node *n = n_queue.top(); // current node
            n_queue.pop();

            if (find(found_leaves.begin(), found_leaves.end(), n) != found_leaves.end()) {
                best_labels.push_back(n);
                p.predictions.push_back(float(n->label));
                p.predictions.push_back(float(n->p));
                if (best_labels.size() >= p.p_at_k) break;
            }
            else {
                float cp = n->p * predict_node(p, n, base, ec);


                if (n->internal) {
                    for (auto child : n->children) {
                        child->p = cp;
                        n_queue.push(child);
                    }
                } else {
                    n->p = cp;
                    found_leaves.push_back(n);
                    n_queue.push(n);
                }
            }
        }

        vector<uint32_t> true_labels;
        for (auto &cl : ec_labels.costs) true_labels.push_back(cl.class_index);

        if (p.p_at_k > 0 && true_labels.size() > 0) {
            for (size_t i = 0; i < p.p_at_k && i < true_labels.size(); ++i) {
                if (find(true_labels.begin(), true_labels.end(), best_labels[i]->label) != true_labels.end())
                    p.precision_at_k[i] += 1.0f;
            }
        }
        
        if(p.top_k_labels) {
            ec.pred.scalars = p.predictions;
        }
    }

    ec.l.cs = ec_labels;
    ++p.ec_count;

    D_COUT << "----------------------------------------------------------------------------------------------------\n";
}


// other
//----------------------------------------------------------------------------------------------------------------------

void finish_example(vw& all, oplt& p, example& ec){

    D_COUT << "FINISH EXAMPLE\n";
    
    if(p.top_k_labels) {
        for (int sink : all.final_prediction_sink){
            MULTILABEL::print_multilabel_with_score(sink, ec.pred.scalars);
        }
    }

    all.sd->update(ec.test_only, 0, ec.weight, ec.num_features);
    VW::finish_example(all, &ec);
}

void pass_end(oplt& p){
    //oplt_print_all_weights(p);

    if(p.pass_count == 0){
        for(auto n : p.tree) n->temp = nullptr;
    }

    ++p.pass_count;
    cerr << "end of pass " << p.pass_count << endl;
}

template<bool use_threshold>
void finish(oplt& p){

    auto end_time_point = chrono::steady_clock::now();
    auto execution_time = end_time_point - p.learn_predict_start_time_point;
    cerr << "learn_predict_time = " << static_cast<double>(chrono::duration_cast<chrono::microseconds>(execution_time).count()) / 1000000 << "s\n";

    if(p.save_tree_structure) save_tree_structure(p, p.save_tree_structure_file);

    // threshold prediction
    if (use_threshold) {
        if (p.predicted_number > 0) {
            cerr << "Precision = " << p.precision / p.predicted_number << "\n";
        } else {
            cerr << "Precision unknown - nothing predicted" << endl;
        }
    }

    // top-k predictions
    else {
        float correct = 0;
        for (size_t i = 0; i < p.p_at_k; ++i) {
            correct += p.precision_at_k[i];
            cerr << "P@" << i + 1 << " = " << correct / (p.prediction_count * (i + 1)) << "\n";
        }
    }

    cerr << "visited nodes = " << p.n_visited_nodes << endl;
    cerr << "tree_size = " << p.tree.size() << endl;
    cerr << "base_predictor_count = " << p.base_predictor_count << endl;

    for(auto n : p.tree) delete n;
    p.tree_leaves.~unordered_map();
}

// setup
//----------------------------------------------------------------------------------------------------------------------

base_learner* oplt_setup(vw& all) //learner setup
{
    if (missing_option<size_t, true>(all, "oplt", "Use online probabilistic label tree for multilabel with max <k> labels"))
        return nullptr;
    new_options(all, "oplt options")
            ("kary_tree", po::value<uint32_t>(), "tree in which each node has no more than k children")
            ("random_policy", "expand random node")
            ("best_prediction_policy", "expand node with best prediction value")
            ("balanced_tree_policy", "keep balanced tree")
            ("complete_tree_policy", "expand random node")
            ("inner_threshold", po::value<float>(), "threshold for positive label (default 0.15)")
            ("p_at", po::value<uint32_t>(), "P@k (default 1)")
            ("positive_labels", "print all positive labels")
            ("top_k_labels", "print top-k labels")
            ("save_tree_structure", po::value<string>(), "save tree structure to file")
            ("load_tree_structure", po::value<string>(), "load tree structure from file")
            ("k_from_structure", "")
            ("greedy", "greedy prediction");
    add_options(all);

    oplt& data = calloc_or_throw<oplt>();
    data.k = all.vm["oplt"].as<size_t>();
    data.kary = 2;
    data.inner_threshold = -1;
    data.positive_labels = false;
    data.top_k_labels = false;
    data.greedy = false;
    data.all = &all;

    data.precision = 0;
    data.predicted_number = 0;
    data.prediction_count = 0;
    data.p_at_k = 1;

    data.rng.seed(all.random_seed);

    data.n_visited_nodes = 0;
    data.ec_count = data.node_count = 0;
    data.pass_count = 0;

    // oplt parse options
    // -----------------------------------------------------------------------------------------------------------------

    learner<oplt> *l;
    string expand_policy = "random_policy";

    // kary options
    if(all.vm.count("kary_tree"))
        data.kary = all.vm["kary_tree"].as<uint32_t>();
    *(all.file_options) << " --kary_tree " << data.kary;

    if(data.kary > 2){
        double a = pow(data.kary, floor(log(data.k) / log(data.kary)));
        double b = data.k - a;
        double c = ceil(b / (data.kary - 1.0));
        double d = (data.kary * a - 1.0)/(data.kary - 1.0);
        double e = data.k - (a - c);
        data.max_predictors = static_cast<uint32_t>(3 * d + 2 * e);
    }
    else
        data.max_predictors = 3 * data.k - 1;
    data.predictor_bits = static_cast<size_t>(floor(log2(data.max_predictors))) + 1;

    // expand policy options
    if(all.vm.count("best_prediction_policy")) {
        data.add_new_label = add_new_label<true, false, false>;
        expand_policy = "best_prediction_policy";
    }
    if(all.vm.count("balanced_tree_policy")) {
        data.add_new_label = add_new_label<false, true, false>;
        expand_policy = "balanced_tree_policy";
    }
    if(all.vm.count("complete_tree_policy")) {
        data.add_new_label = add_new_label<false, false, true>;
        expand_policy = "complete_tree_policy";
    }
    else // if(all.vm.count("random_policy"))
        data.add_new_label = add_new_label<false, false, false>;


    if(all.vm.count("inner_threshold"))
        data.inner_threshold = all.vm["inner_threshold"].as<float>();

    if(all.vm.count("p_at") )
        data.p_at_k = all.vm["p_at"].as<uint32_t>();

    if(all.vm.count("positive_labels"))
        data.positive_labels = true;

    if(all.vm.count("top_k_labels"))
        data.top_k_labels = true;

    if( all.vm.count("greedy"))
        data.greedy = true;

    if(all.weights.stride_shift())
        data.copy = copy_weights<false>;
    else
        data.copy = copy_weights<true>;


    // init tree
    // -----------------------------------------------------------------------------------------------------------------

    init_tree(data);

    if(all.vm.count("save_tree_structure")) {
        data.save_tree_structure = true;
        data.save_tree_structure_file = all.vm["save_tree_structure"].as<string>();
    }
    else
        data.save_tree_structure = false;
    if(all.vm.count("load_tree_structure"))
        load_tree_structure(data, all.vm["load_tree_structure"].as<string>());

    if(all.vm.count("k_from_structure"))
        data.max_predictors = data.tree.size();


    // init multiclass learner
    // -----------------------------------------------------------------------------------------------------------------

    if (data.inner_threshold >= 0) {
        l = &init_multiclass_learner(&data, setup_base(all), learn, predict<true, false>, all.p, data.max_predictors);
        l->set_finish(finish<true>);
    }
    else if(data.greedy){
        data.p_at_k = 1;
        data.precision_at_k.resize(data.p_at_k);
        l = &init_multiclass_learner(&data, setup_base(all), learn, predict<false, true>, all.p, data.max_predictors);
        l->set_finish(finish<false>);
    }
    else{
        data.precision_at_k.resize(data.p_at_k);
        l = &init_multiclass_learner(&data, setup_base(all), learn, predict<false, false>, all.p, data.max_predictors);
        l->set_finish(finish<false>);
    }

    // override parser
    // -----------------------------------------------------------------------------------------------------------------

    all.p->lp = COST_SENSITIVE::cs_label;
    all.cost_sensitive = make_base(*l);

    all.holdout_set_off = true; // turn off stop based on holdout loss


    // log info & add some event handlers
    // -----------------------------------------------------------------------------------------------------------------

    cerr << "oplt\n" << "max_lables = " << data.k << "\npredictor_bits = " << data.predictor_bits << "\nmax_predictors = " << data.max_predictors
         << "\nkary_tree = " << data.kary << "\ninner_threshold = " << data.inner_threshold << endl;

    if(expand_policy.length())
        cerr << expand_policy << endl;

    l->set_finish_example(finish_example);
    l->set_save_load(save_load_tree);
    l->set_end_pass(pass_end);

    return all.cost_sensitive;
}
