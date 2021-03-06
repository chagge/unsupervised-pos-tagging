#include <boost/format.hpp>
#include <boost/python.hpp>
#include <boost/python/tuple.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <string>
#include <set>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <cassert>
#include "core/hpylm.h"
#include "core/lattice.h"
#include "core/util.h"
using namespace std;
using namespace boost;

struct value_comparator {
	bool operator()(const pair<int, int> &a, const pair<int, int> &b) {
		return a.second > b.second;
	}   
};

class PyHpylmHMM{
private:
	HPYLM** _word_hpylm_for_tag;
	HPYLM* _pos_hpylm;
	Lattice* _lattice;
	unordered_map<int, wstring> _dictionary;
	unordered_map<wstring, int> _dictionary_inv;
	vector<vector<Word*>> _train_dataset;
	vector<vector<Word*>> _test_dataset;
	vector<int> _rand_indices;
	set<int> _types_of_words;
	int _autoincrement;
	int _num_tags;
	int _max_num_words_in_sentence;	// 1文あたりの最大単語数
	bool _is_ready;
	bool _is_first_run;
public:
	PyHpylmHMM(int num_tags){
		// 日本語周り
		// ただのテンプレ
		setlocale(LC_CTYPE, "ja_JP.UTF-8");
		ios_base::sync_with_stdio(false);
		locale default_loc("ja_JP.UTF-8");
		locale::global(default_loc);
		locale ctype_default(locale::classic(), default_loc, locale::ctype); //※
		wcout.imbue(ctype_default);
		wcin.imbue(ctype_default);

		// HPYLMは全て3-gram
		_pos_hpylm = new HPYLM(3);
		_word_hpylm_for_tag = new HPYLM*[num_tags];
		for(int tag = 0;tag < num_tags;tag++){
			_word_hpylm_for_tag[tag] = new HPYLM(3);
		}
		_lattice = NULL;
		_num_tags = num_tags;
		_dictionary[BEGIN_OF_SENTENSE] = L"<bos>";
		_dictionary[END_OF_SENTENSE] = L"<eos>";
		_autoincrement = END_OF_SENTENSE + 1;
		_max_num_words_in_sentence = 0;
		_is_ready = false;
	}
	~PyHpylmHMM(){
		delete _pos_hpylm;
		for(int tag = 0;tag < _num_tags;tag++){
			delete _word_hpylm_for_tag[tag];
		}
		delete[] _word_hpylm_for_tag;
	}
	int string_to_word_id(wstring &word){
		auto itr = _dictionary_inv.find(word);
		if(itr == _dictionary_inv.end()){
			_dictionary[_autoincrement] = word;
			_dictionary_inv[word] = _autoincrement;
			_autoincrement++;
			return _autoincrement - 1;
		}
		return itr->second;
	}
	void load_textfile(string filename, double split_probability=0.05){
		c_printf("[*]%s\n", (boost::format("%sを読み込んでいます ...") % filename.c_str()).str().c_str());
		wifstream ifs(filename.c_str());
		wstring line_str;
		if (ifs.fail()){
			c_printf("[R]%s [*]%s", "エラー", (boost::format("%sを開けません.") % filename.c_str()).str().c_str());
			exit(1);
		}
		int prev_train_dataset_size = _train_dataset.size();
		int prev_test_dataset_size = _test_dataset.size();
		while (getline(ifs, line_str) && !line_str.empty()){
			vector<wstring> word_strs = split_word_by(line_str, L' ');	// スペースで分割
			if(word_strs.size() > 0){
				vector<Word*> words;
				// <bos>
				// 3-gramなので2つ
				for(int n = 0;n < 2;n++){
					Word* bos = new Word();
					bos->word_id = BEGIN_OF_SENTENSE;
					bos->tag_id = BEGIN_OF_POS;
					words.push_back(bos);
				}
				for(auto &word_str: word_strs){
					if(word_str.size() == 0){
						continue;
					}
					Word* word = new Word();
					word->word_id = string_to_word_id(word_str);
					word->tag_id = 0;
					words.push_back(word);
					_types_of_words.insert(word->word_id);
				}
				Word* eos = new Word();
				eos->word_id = END_OF_SENTENSE;
				eos->tag_id = END_OF_POS;
				words.push_back(eos);
				// 単語数を記録しておく
				if(words.size() > _max_num_words_in_sentence){
					_max_num_words_in_sentence = words.size();
				}
				// 訓練データかテストデータどちらに含めるかを決める
				double bernoulli = Sampler::uniform(0, 1);
				if(bernoulli < split_probability){
					_test_dataset.push_back(words);
				}else{
					_train_dataset.push_back(words);
				}
			}
		}
		c_printf("[*]%s\n", (boost::format("%sを読み込みました.") % filename.c_str()).str().c_str());
		c_printf("[*]%s\n", (boost::format("訓練データ: %d行, テストデータ: %d行, 単語数: %d") % (_train_dataset.size() - prev_train_dataset_size) % (_test_dataset.size() - prev_test_dataset_size) % _types_of_words.size()).str().c_str());
	}
	void load(string dirname){
		// 辞書を読み込み
		string dictionary_filename = dirname + "/hmm.dict";
		std::ifstream ifs(dictionary_filename);
		if(ifs.good()){
			boost::archive::binary_iarchive iarchive(ifs);
			iarchive >> _dictionary;
			ifs.close();
		}
		// モデルパラメータの読み込み
		_pos_hpylm->load(dirname + "/pos.hpylm");
		for(int tag = 0;tag < _num_tags;tag++){
			_word_hpylm_for_tag[tag]->load((boost::format("%s/word.%d.hpylm") % dirname.c_str() % tag).str());
		}
	}
	void save(string dirname){
		// 辞書を保存
		string dictionary_filename = dirname + "/hmm.dict";
		std::ofstream ofs(dictionary_filename);
		boost::archive::binary_oarchive oarchive(ofs);
		oarchive << _dictionary;
		ofs.close();
		// モデルパラメータの保存
		_pos_hpylm->save(dirname + "/pos.hpylm");
		for(int tag = 0;tag < _num_tags;tag++){
			_word_hpylm_for_tag[tag]->save((boost::format("%s/word.%d.hpylm") % dirname.c_str() % tag).str());
		}
	}
	void prepare_for_training(){
		c_printf("[*]%s\n", "学習の準備中です ...");
		if(_lattice == NULL){
			_lattice = new Lattice(_max_num_words_in_sentence, _num_tags, _pos_hpylm, _word_hpylm_for_tag);
		}
		if(_rand_indices.size() != _train_dataset.size()){
			_rand_indices.clear();
			for(int data_index = 0;data_index < _train_dataset.size();data_index++){
				_rand_indices.push_back(data_index);
			}
		}
		// 基底分布G0を設定
		assert(_pos_hpylm != NULL);
		_pos_hpylm->set_g0(1.0 / _num_tags);
		for(int tag = 0;tag < _num_tags;tag++){
			HPYLM* word_hpylm = _word_hpylm_for_tag[tag];
			assert(word_hpylm != NULL);
			word_hpylm->set_g0(1.0 / get_num_types_of_word());
		}
		// 各文にランダムに品詞を割り当ててモデルに追加
		vector<int> token_ids = {0, 0, 0};
		for(int data_index = 0;data_index < _train_dataset.size();data_index++){
			if (PyErr_CheckSignals() != 0) {		// ctrl+cが押されたかチェック
				return;
			}
			vector<Word*> &sentence = _train_dataset[data_index];
			// HPYLMを更新
			for(int t = 2;t < sentence.size();t++){
				sentence[t]->tag_id = Sampler::uniform_int(0, _num_tags - 1);
				generate_pos_token_ids(sentence, token_ids, t);
				_pos_hpylm->add_customer_at_timestep(token_ids, 2);
				generate_word_token_ids(sentence, token_ids, t);
				int tag = sentence[t]->tag_id;
				HPYLM* hpylm = _word_hpylm_for_tag[tag];
				hpylm->add_customer_at_timestep(token_ids, 2);
			}
			// プログレスバー
			if(data_index % 100 == 0 || data_index == _train_dataset.size() - 1){
				show_progress(data_index, _train_dataset.size());
			}
		}
		cout << "\r\33[2K";
		_is_ready = true;
	}
	void generate_pos_token_ids(vector<Word*> &sentence, vector<int> &token_ids, int t){
		token_ids[0] = sentence[t - 2]->tag_id;
		token_ids[1] = sentence[t - 1]->tag_id;
		token_ids[2] = sentence[t]->tag_id;
	}
	void generate_word_token_ids(vector<Word*> &sentence, vector<int> &token_ids, int t){
		token_ids[0] = sentence[t - 2]->word_id;
		token_ids[1] = sentence[t - 1]->word_id;
		token_ids[2] = sentence[t]->word_id;
	}
	void perform_gibbs_sampling(){
		assert(_is_ready);
		assert(_rand_indices.size() == _train_dataset.size());
		shuffle(_rand_indices.begin(), _rand_indices.end(), Sampler::mt);	// データをシャッフル
		vector<int> token_ids = {0, 0, 0};
		for(int n = 0;n < _train_dataset.size();n++){
			if (PyErr_CheckSignals() != 0) {		// ctrl+cが押されたかチェック
				return;
			}
			int data_index = _rand_indices[n];
			vector<Word*> &sentence = _train_dataset[data_index];
			// 以前のサンプリング結果を削除
			for(int t = 2;t < sentence.size();t++){
				generate_pos_token_ids(sentence, token_ids, t);
				_pos_hpylm->remove_customer_at_timestep(token_ids, 2);
				generate_word_token_ids(sentence, token_ids, t);
				int tag = sentence[t]->tag_id;
				HPYLM* hpylm = _word_hpylm_for_tag[tag];
				hpylm->remove_customer_at_timestep(token_ids, 2);
			}
			// 品詞をサンプリングしてセット
			_lattice->perform_blocked_gibbs_sampling(sentence, false);	// argmax=false
			// HPYLMを更新
			for(int t = 2;t < sentence.size();t++){
				generate_pos_token_ids(sentence, token_ids, t);
				_pos_hpylm->add_customer_at_timestep(token_ids, 2);
				generate_word_token_ids(sentence, token_ids, t);
				int tag = sentence[t]->tag_id;
				HPYLM* hpylm = _word_hpylm_for_tag[tag];
				hpylm->add_customer_at_timestep(token_ids, 2);
			}
			// プログレスバー
			if(n % 100 == 0 || n == _train_dataset.size() - 1){
				show_progress(n, _train_dataset.size());
			}
		}
		_is_first_run = false;
	}
	// デバッグ用
	void remove_all_customers(){
		assert(_is_ready);
		vector<int> token_ids = {0, 0, 0};
		for(int data_index = 0;data_index < _train_dataset.size();data_index++){
			if (PyErr_CheckSignals() != 0) {		// ctrl+cが押されたかチェック
				return;
			}
			vector<Word*> &sentence = _train_dataset[data_index];
			// 以前のサンプリング結果を削除
			for(int t = 2;t < sentence.size();t++){
				generate_pos_token_ids(sentence, token_ids, t);
				_pos_hpylm->remove_customer_at_timestep(token_ids, 2);
				generate_word_token_ids(sentence, token_ids, t);
				int tag = sentence[t]->tag_id;
				HPYLM* hpylm = _word_hpylm_for_tag[tag];
				hpylm->remove_customer_at_timestep(token_ids, 2);
			}
		}
	}
	void sample_hyperparams(){
		_pos_hpylm->sample_hyperparams();
		for(int tag = 0;tag < _num_tags;tag++){
			_word_hpylm_for_tag[tag]->sample_hyperparams();
		}
	}
	int get_num_types_of_word(){
		return _types_of_words.size();
	}
	int get_num_tags(){
		return _num_tags;
	}
	double compute_perplexity(){
		double ppl = 0;
		int num_lines = _test_dataset.size();
		vector<int> context_token_ids = {0, 0};
		for(int data_index = 0;data_index < num_lines;data_index++){
			vector<Word*> &sentence = _test_dataset[data_index];
			double log_Pw = 0;
			for(int t = 2;t < sentence.size();t++){
				context_token_ids[0] = sentence[t - 2]->word_id;
				context_token_ids[1] = sentence[t - 1]->word_id;
				cout << context_token_ids[0] << "," << context_token_ids[1] << endl;
				int tag = sentence[t]->tag_id;
				HPYLM* hpylm = _word_hpylm_for_tag[tag];
				double Pw_h = hpylm->compute_Pw_h(token_id, context_token_ids);
				log_Pw += log2(Pw_h);
			}
			ppl += log_Pw / (sentence.size() - 2);
		}
		ppl = exp(-ppl / num_lines);
		return ppl;
	}
	void dump_hpylm(){
		for(int tag = 0;tag < _num_tags;tag++){
			HPYLM* hpylm = _word_hpylm_for_tag[tag];
			cout << "hpylm: " << tag << endl;
			cout << "# of customers: " << hpylm->get_num_customers() << endl;
		}
		cout << "hpylm (pos)" << endl;
		cout << "# of customers: " << _pos_hpylm->get_num_customers() << endl;
	}
	void show_typical_words_for_each_tag(int number_to_show_for_each_tag){
		for(int tag = 0;tag < _num_tags;tag++){
			int n = 0;
			wcout << L"tag " << tag << L":" << endl << L"	";
			multiset<pair<int, int>, value_comparator> ranking;
			unordered_map<int, vector<int>> &arrangement = _word_hpylm_for_tag[tag]->_root->_arrangement;
			for(auto &elem: arrangement){
				int token_id = elem.first;
				vector<int> &num_customers_at_table = elem.second;
				int count = std::accumulate(num_customers_at_table.begin(), num_customers_at_table.end(), 0);	
				ranking.insert(std::make_pair(token_id, count));
			}
			for(auto elem: ranking){
				wstring word = _dictionary[elem.first];
				wcout << word << L"/" << elem.second << L", ";
				n++;
				if(n > number_to_show_for_each_tag){
					break;
				}
				if(elem.second < 10){
					break;
				}
			}
			wcout << endl;
		}
	}
};

BOOST_PYTHON_MODULE(model){
	python::class_<PyHpylmHMM>("hpylm_hmm", python::init<int>())
	.def("string_to_word_id", &PyHpylmHMM::string_to_word_id)
	.def("prepare_for_training", &PyHpylmHMM::prepare_for_training)
	.def("perform_gibbs_sampling", &PyHpylmHMM::perform_gibbs_sampling)
	.def("sample_hyperparams", &PyHpylmHMM::sample_hyperparams)
	.def("compute_perplexity", &PyHpylmHMM::compute_perplexity)
	.def("load", &PyHpylmHMM::load)
	.def("save", &PyHpylmHMM::save)
	.def("get_num_tags", &PyHpylmHMM::get_num_tags)
	.def("dump_hpylm", &PyHpylmHMM::dump_hpylm)
	.def("show_typical_words_for_each_tag", &PyHpylmHMM::show_typical_words_for_each_tag)
	.def("remove_all_customers", &PyHpylmHMM::remove_all_customers)
	.def("load_textfile", &PyHpylmHMM::load_textfile);
}