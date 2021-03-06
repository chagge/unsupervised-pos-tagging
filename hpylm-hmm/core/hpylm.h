#ifndef _hpylm_
#define _hpylm_
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <vector>
#include <random>
#include <unordered_map> 
#include <cstdlib>
#include <cassert>
#include "cprintf.h"
#include "node.h"
#include "const.h"
#include "sampler.h"

class HPYLM{
private:
	int _hpylm_depth;				// 最大の深さ
	friend class boost::serialization::access;
	template <class Archive>
	// モデルの保存
	void serialize(Archive& archive, unsigned int version)
	{
		static_cast<void>(version); // No use
		archive & _root;
		archive & _hpylm_depth;
		archive & _g0;
		archive & _d_m;
		archive & _theta_m;
		archive & _a_m;
		archive & _b_m;
		archive & _alpha_m;
		archive & _beta_m;
	}
public:
	Node* _root;				// 文脈木のルートノード
	int _max_depth;				// VPYLMへ拡張時に使う
	double _g0;					// ゼログラム確率

	// 深さmのノードに関するパラメータ
	vector<double> _d_m;		// Pitman-Yor過程のディスカウント係数
	vector<double> _theta_m;	// Pitman-Yor過程の集中度

	// "A Bayesian Interpretation of Interpolated Kneser-Ney" Appendix C参照
	// http://www.gatsby.ucl.ac.uk/~ywteh/research/compling/hpylm.pdf
	vector<double> _a_m;		// ベータ分布のパラメータ	dの推定用
	vector<double> _b_m;		// ベータ分布のパラメータ	dの推定用
	vector<double> _alpha_m;	// ガンマ分布のパラメータ	θの推定用
	vector<double> _beta_m;		// ガンマ分布のパラメータ	θの推定用

	HPYLM(int ngram = 2){
		// 深さは0から始まることに注意
		// 2-gramなら最大深さは1. root(0) -> 2-gram(1)
		// 3-gramなら最大深さは2. root(0) -> 2-gram(1) -> 3-gram(2)
		_hpylm_depth = ngram - 1;
		_max_depth = -1;

		_root = new Node();
		_root->_depth = 0;		// ルートは深さ0

		for(int n = 0;n < ngram;n++){
			_d_m.push_back(PYLM_INITIAL_D);	
			_theta_m.push_back(PYLM_INITIAL_THETA);
			_a_m.push_back(PYLM_INITIAL_A);	
			_b_m.push_back(PYLM_INITIAL_B);	
			_alpha_m.push_back(PYLM_INITIAL_ALPHA);
			_beta_m.push_back(PYLM_INITIAL_BETA);
		}
	}
	int ngram(){
		return _hpylm_depth + 1;
	}
	void set_g0(double g0){
		_g0 = g0;
	}
	// 単語列のindex番目の単語をモデルに追加
	bool add_customer_at_timestep(vector<int> &token_ids, int token_t_index){
		assert(token_ids.size() > token_t_index);
		Node* node = find_node_by_tracing_back_context(token_ids, token_t_index, _hpylm_depth, true);
		if(node == NULL){
			c_printf("[r]%s [*]%s\n", "エラー:", "客を追加できません. ノードが見つかりません.");
			exit(1);
		}
		int token_t = token_ids[token_t_index];
		int added_to_table_k;
		node->add_customer(token_t, _g0, _d_m, _theta_m, true, added_to_table_k);
		return true;
	}
	bool remove_customer_at_timestep(vector<int> &token_ids, int token_t_index){
		assert(token_ids.size() > token_t_index);
		Node* node = find_node_by_tracing_back_context(token_ids, token_t_index, _hpylm_depth, false);
		if(node == NULL){
			c_printf("[r]%s [*]%s\n", "エラー:", "客を除去できません. ノードが見つかりません.");
			exit(1);
		}
		int token_t = token_ids[token_t_index];
		int removed_from_table_k;
		node->remove_customer(token_t, true, removed_from_table_k);
		// 客が一人もいなくなったらノードを削除する
		if(node->need_to_remove_from_parent()){
			node->remove_from_parent();
		}
		return true;
	}
	// token列の位置tからorderだけ遡る
	// token_ids:        [0, 1, 2, 3, 4, 5]
	// token_t_index:4          ^     ^
	// depth_t: 2               |<- <-|
	Node* find_node_by_tracing_back_context(vector<int> &token_ids, int token_t_index, int depth_t, bool generate_node_if_needed = false, bool return_middle_node = false){
		if(token_t_index - depth_t < 0){
			return NULL;
		}
		Node* node = _root;
		for(int depth = 1;depth <= depth_t;depth++){
			int context_token_id = token_ids[token_t_index - depth];
			Node* child = node->find_child_node(context_token_id, generate_node_if_needed);
			if(child == NULL){
				if(return_middle_node){
					return node;
				}
				return NULL;
			}
			node = child;
		}
		return node;
	}
	double compute_Pw_h(vector<int> &token_ids, vector<int> context_token_ids){
		double p = 1;
		for(int n = 0;n < token_ids.size();n++){
			p *= compute_Pw_h(token_ids[n], context_token_ids);
			context_token_ids.push_back(token_ids[n]);
		}
		return p;
	}
	// 式に忠実な実装
	double _compute_Pw_h(int token_id, vector<int> &context_token_ids){
		// HPYLMでは深さは固定
		if(context_token_ids.size() < _hpylm_depth){
			c_printf("[r]%s [*]%s\n", "エラー:", "単語確率を計算できません. context_token_ids.size() < _hpylm_depth");
			exit(1);
		}
		Node* node = find_node_by_tracing_back_context(context_token_ids, context_token_ids.size(), _hpylm_depth, false, true);
		if(node == NULL){
			c_printf("[r]%s [*]%s\n", "エラー:", "単語確率を計算できません. node == NULL");
			exit(1);
		}
		return node->compute_Pw(token_id, _g0, _d_m, _theta_m);
	}
	// 効率化したバージョン
	// 親の確率を計算しながら子を辿る
	double compute_Pw_h(int token_id, vector<int> &context_token_ids){
		assert(context_token_ids.size() >= _hpylm_depth);
		double parent_Pw = _g0;
		Node* node = _root;
		for(int depth = 1;depth <= _hpylm_depth;depth++){
			int context_token_id = context_token_ids[2 - depth];
			Node* child = node->find_child_node(context_token_id, false);
			parent_Pw = node->_compute_Pw(token_id, parent_Pw, _d_m, _theta_m);
			if(child == NULL){
				return parent_Pw;
			}
			node = child;
		}
		return node->_compute_Pw(token_id, parent_Pw, _d_m, _theta_m);
	}
	double compute_Pw(int token_id){
		return _root->compute_Pw(token_id, _g0, _d_m, _theta_m);
	}
	double compute_Pw(vector<int> &token_ids){
		if(token_ids.size() < _hpylm_depth + 1){
			c_printf("[r]%s [*]%s\n", "エラー:", "単語確率を計算できません. token_ids.size() < _hpylm_depth");
			exit(1);
		}
		double mul_Pw_h = 1;
		vector<int> context_token_ids(token_ids.begin(), token_ids.begin() + _hpylm_depth);
		for(int depth = _hpylm_depth;depth < token_ids.size();depth++){
			int token_id = token_ids[depth];
			mul_Pw_h *= compute_Pw_h(token_id, context_token_ids);;
			context_token_ids.push_back(token_id);
		}
		return mul_Pw_h;
	}
	double compute_log_Pw(vector<int> &token_ids){
		if(token_ids.size() < _hpylm_depth + 1){
			c_printf("[r]%s [*]%s\n", "エラー:", "単語確率を計算できません. token_ids.size() < _hpylm_depth");
			exit(1);
		}
		double sum_Pw_h = 0;
		vector<int> context_token_ids(token_ids.begin(), token_ids.begin() + _hpylm_depth);
		for(int depth = _hpylm_depth;depth < token_ids.size();depth++){
			int token_id = token_ids[depth];
			sum_Pw_h += log(compute_Pw_h(token_id, context_token_ids) + 1e-10);
			context_token_ids.push_back(token_id);
		}
		return sum_Pw_h;
	}
	double compute_log2_Pw(vector<int> &token_ids){
		if(token_ids.size() < _hpylm_depth + 1){
			c_printf("[r]%s [*]%s\n", "エラー:", "単語確率を計算できません. token_ids.size() < _hpylm_depth");
			exit(1);
		}
		double sum_Pw_h = 0;
		vector<int> context_token_ids(token_ids.begin(), token_ids.begin() + _hpylm_depth);
		for(int depth = _hpylm_depth;depth < token_ids.size();depth++){
			int token_id = token_ids[depth];
			sum_Pw_h += log2(compute_Pw_h(token_id, context_token_ids) + 1e-10);
			context_token_ids.push_back(token_id);
		}
		return sum_Pw_h;
	}
	void init_hyperparameters_at_depth_if_needed(int depth){
		if(depth >= _d_m.size()){
			while(_d_m.size() <= depth){
				_d_m.push_back(PYLM_INITIAL_D);
			}
		}
		if(depth >= _theta_m.size()){
			while(_theta_m.size() <= depth){
				_theta_m.push_back(PYLM_INITIAL_THETA);
			}
		}
		if(depth >= _a_m.size()){
			while(_a_m.size() <= depth){
				_a_m.push_back(PYLM_INITIAL_A);
			}
		}
		if(depth >= _b_m.size()){
			while(_b_m.size() <= depth){
				_b_m.push_back(PYLM_INITIAL_B);
			}
		}
		if(depth >= _alpha_m.size()){
			while(_alpha_m.size() <= depth){
				_alpha_m.push_back(PYLM_INITIAL_ALPHA);
			}
		}
		if(depth >= _beta_m.size()){
			while(_beta_m.size() <= depth){
				_beta_m.push_back(PYLM_INITIAL_BETA);
			}
		}
	}
	// "A Bayesian Interpretation of Interpolated Kneser-Ney" Appendix C参照
	// http://www.gatsby.ucl.ac.uk/~ywteh/research/compling/hpylm.pdf
	void sum_auxiliary_variables_recursively(Node* node, vector<double> &sum_log_x_u_m, vector<double> &sum_y_ui_m, vector<double> &sum_1_y_ui_m, vector<double> &sum_1_z_uwkj_m, int &bottom){
		for(const auto &elem: node->_children){
			Node* child = elem.second;
			int depth = child->_depth;

			if(depth > bottom){
				bottom = depth;
			}
			init_hyperparameters_at_depth_if_needed(depth);

			double d = _d_m[depth];
			double theta = _theta_m[depth];
			sum_log_x_u_m[depth] += child->auxiliary_log_x_u(theta);	// log(x_u)
			sum_y_ui_m[depth] += child->auxiliary_y_ui(d, theta);		// y_ui
			sum_1_y_ui_m[depth] += child->auxiliary_1_y_ui(d, theta);	// 1 - y_ui
			sum_1_z_uwkj_m[depth] += child->auxiliary_1_z_uwkj(d);		// 1 - z_uwkj

			sum_auxiliary_variables_recursively(child, sum_log_x_u_m, sum_y_ui_m, sum_1_y_ui_m, sum_1_z_uwkj_m, bottom);
		}
	}
	// dとθの推定
	void sample_hyperparams(){
		int max_depth = _d_m.size() - 1;

		// 親ノードの深さが0であることに注意
		vector<double> sum_log_x_u_m(max_depth + 1, 0.0);
		vector<double> sum_y_ui_m(max_depth + 1, 0.0);
		vector<double> sum_1_y_ui_m(max_depth + 1, 0.0);
		vector<double> sum_1_z_uwkj_m(max_depth + 1, 0.0);

		// _root
		sum_log_x_u_m[0] = _root->auxiliary_log_x_u(_theta_m[0]);			// log(x_u)
		sum_y_ui_m[0] = _root->auxiliary_y_ui(_d_m[0], _theta_m[0]);		// y_ui
		sum_1_y_ui_m[0] = _root->auxiliary_1_y_ui(_d_m[0], _theta_m[0]);	// 1 - y_ui
		sum_1_z_uwkj_m[0] = _root->auxiliary_1_z_uwkj(_d_m[0]);				// 1 - z_uwkj

		// それ以外
		_max_depth = 0;
		// _max_depthはsum_auxiliary_variables_recursivelyを実行すると正確な値に更新にされる
		// HPYLMでは無意味だがVPYLMで最大深さを求める時に使う
		sum_auxiliary_variables_recursively(_root, sum_log_x_u_m, sum_y_ui_m, sum_1_y_ui_m, sum_1_z_uwkj_m, _max_depth);
		init_hyperparameters_at_depth_if_needed(_max_depth);

		for(int u = 0;u <= _max_depth;u++){
			_d_m[u] = Sampler::beta(_a_m[u] + sum_1_y_ui_m[u], _b_m[u] + sum_1_z_uwkj_m[u]);
			_theta_m[u] = Sampler::gamma(_alpha_m[u] + sum_y_ui_m[u], _beta_m[u] - sum_log_x_u_m[u]);
		}
		// 不要な深さのハイパーパラメータを削除
		// HPYLMでは無意味
		int num_remove = _d_m.size() - _max_depth - 1;
		for(int n = 0;n < num_remove;n++){
			_d_m.pop_back();
			_theta_m.pop_back();
			_a_m.pop_back();
			_b_m.pop_back();
			_alpha_m.pop_back();
			_beta_m.pop_back();
		}
	}
	int get_max_depth(bool use_cache = true){
		if(use_cache && _max_depth != -1){
			return _max_depth;
		}
		_max_depth = 0;
		update_max_depth_recursively(_root);
		return _max_depth;
	}
	void update_max_depth_recursively(Node* node){
		for(const auto &elem: node->_children){
			Node* child = elem.second;
			int depth = child->_depth;
			if(depth > _max_depth){
				_max_depth = depth;
			}
			update_max_depth_recursively(child);
		}
	}
	int get_num_nodes(){
		return _root->get_num_nodes() + 1;
	}
	int get_num_customers(){
		return _root->get_num_customers();
	}
	int get_num_tables(){
		return _root->get_num_tables();
	}
	int get_sum_stop_counts(){
		return _root->sum_stop_counts();
	}
	int get_sum_pass_counts(){
		return _root->sum_pass_counts();
	}
	void count_tokens_of_each_depth(unordered_map<int, int> &map){
		_root->count_tokens_of_each_depth(map);
	}
	bool save(string filename = "hpylm.model"){
		std::ofstream ofs(filename);
		boost::archive::binary_oarchive oarchive(ofs);
		oarchive << static_cast<const HPYLM&>(*this);
		ofs.close();
		return true;
	}
	bool load(string filename = "hpylm.model"){
		std::ifstream ifs(filename);
		if(ifs.good() == false){
			return false;
		}
		boost::archive::binary_iarchive iarchive(ifs);
		iarchive >> *this;
		ifs.close();
		return true;
	}
};

#endif