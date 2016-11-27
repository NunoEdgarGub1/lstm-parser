#ifndef LSTM_PARSER_H
#define LSTM_PARSER_H

#include <boost/serialization/unordered_map.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cnn/training.h"
#include "cnn/cnn.h"
#include "cnn/expr.h"
#include "cnn/nodes.h"
#include "cnn/lstm.h"
#include "cnn/rnn.h"
#include "corpus.h"


namespace lstm_parser {

struct ParserOptions {
  bool use_pos;
  unsigned layers;
  unsigned input_dim;
  unsigned hidden_dim;
  unsigned action_dim;
  unsigned lstm_input_dim;
  unsigned pos_dim;
  unsigned rel_dim;
  unsigned unk_strategy;

  template<class Archive>
  void serialize(Archive & ar, const unsigned int version) {
    ar & use_pos;
    ar & layers;
    ar & input_dim;
    ar & hidden_dim;
    ar & action_dim;
    ar & lstm_input_dim;
    ar & pos_dim;
    ar & rel_dim;
    ar & unk_strategy;
  }

  inline bool operator==(const ParserOptions& other) const {
    return use_pos == other.use_pos && layers == other.layers
        && input_dim == other.input_dim && hidden_dim == other.hidden_dim
        && action_dim == other.action_dim
        && lstm_input_dim == other.lstm_input_dim && pos_dim == other.pos_dim
        && rel_dim == other.rel_dim && unk_strategy == other.unk_strategy;
  }

  inline bool operator!=(const ParserOptions& other) const {
    return !operator==(other);
  }
};


class ParseTree {
public:
  // Barebones representation of a parse tree.
  const std::vector<unsigned>& sentence;

  ParseTree(const std::vector<unsigned>& sentence, bool labeled = true) :
      sentence(sentence),
      parents(sentence.size(), -1),
      arc_labels( labeled ? new std::vector<std::string>(sentence.size(),
                                                         "ERROR") : nullptr) {
  }

  inline void SetParent(unsigned index, unsigned parent_index,
                      const std::string& arc_label="") {
    parents[index] = parent_index;
    if (arc_labels) {
      (*arc_labels)[index] = arc_label;
    }
  }

  const inline std::vector<int>& GetParents() const { return parents; }
  const inline std::vector<std::string>& GetArcLabels() const {
    return *arc_labels;
  }

private:
  std::vector<int> parents;
  std::unique_ptr<std::vector<std::string>> arc_labels;
};


class LSTMParser {
public:
  // TODO: make some of these members non-public
  static constexpr const char* ROOT_SYMBOL = "ROOT";

  ParserOptions options;
  CorpusVocabulary vocab;
  cnn::Model model;

  bool finalized;
  std::unordered_map<unsigned, std::vector<float>> pretrained;
  unsigned n_possible_actions;
  const unsigned kUNK;
  const unsigned kROOT_SYMBOL;

  cnn::LSTMBuilder stack_lstm; // (layers, input, hidden, trainer)
  cnn::LSTMBuilder buffer_lstm;
  cnn::LSTMBuilder action_lstm;
  cnn::LookupParameters* p_w; // word embeddings
  cnn::LookupParameters* p_t; // pretrained word embeddings (not updated)
  cnn::LookupParameters* p_a; // input action embeddings
  cnn::LookupParameters* p_r; // relation embeddings
  cnn::LookupParameters* p_p; // pos tag embeddings
  cnn::Parameters* p_pbias; // parser state bias
  cnn::Parameters* p_A; // action lstm to parser state
  cnn::Parameters* p_B; // buffer lstm to parser state
  cnn::Parameters* p_S; // stack lstm to parser state
  cnn::Parameters* p_H; // head matrix for composition function
  cnn::Parameters* p_D; // dependency matrix for composition function
  cnn::Parameters* p_R; // relation matrix for composition function
  cnn::Parameters* p_w2l; // word to LSTM input
  cnn::Parameters* p_p2l; // POS to LSTM input
  cnn::Parameters* p_t2l; // pretrained word embeddings to LSTM input
  cnn::Parameters* p_ib; // LSTM input bias
  cnn::Parameters* p_cbias; // composition function bias
  cnn::Parameters* p_p2a;   // parser state to action
  cnn::Parameters* p_action_start;  // action bias
  cnn::Parameters* p_abias;  // action bias
  cnn::Parameters* p_buffer_guard;  // end of buffer
  cnn::Parameters* p_stack_guard;  // end of stack

  explicit LSTMParser(const ParserOptions& options,
                         const std::string& pretrained_words_path,
                         bool finalize=true);

  static bool IsActionForbidden(const std::string& a, unsigned bsize,
                                unsigned ssize, const std::vector<int>& stacki);

  // take a std::vector of actions and return a parse tree
  static ParseTree RecoverParseTree(
      const std::vector<unsigned>& sentence,
      const std::vector<unsigned>& actions,
      const std::vector<std::string>& action_names, bool labeled = false);

  void Train(const Corpus& corpus, const Corpus& dev_corpus,
             const double unk_prob, const std::string& model_fname,
             bool compress, const volatile bool* requested_stop=nullptr);

  void Test(const Corpus& corpus);

  // *** if correct_actions is empty, this runs greedy decoding ***
  // returns parse actions for input sentence (in training just returns the
  // reference)
  // OOV handling: raw_sent will have the actual words
  //               sent will have words replaced by appropriate UNK tokens
  // this lets us use pretrained embeddings, when available, for words that were
  // OOV in the parser training data.
  std::vector<unsigned> LogProbParser(
      cnn::ComputationGraph* hg,
      const std::vector<unsigned>& raw_sent,  // raw sentence
      const std::vector<unsigned>& sent,  // sentence with OOVs replaced
      const std::vector<unsigned>& sentPos,
      const std::vector<unsigned>& correct_actions,
      const std::vector<std::string>& setOfActions,
      const std::vector<std::string>& intToWords, double* right);

  void LoadPretrainedWords(const std::string& words_path);

  void FinalizeVocab();

protected:
  void SaveModel(const std::string& model_fname, bool compress,
                 bool softlinkCreated);

  inline unsigned ComputeCorrect(const ParseTree& ref,
                                 const ParseTree& hyp) const {
    assert(ref.sentence.size() == hyp.sentence.size());
    unsigned correct_count = 0;
    for (unsigned i = 0; i < ref.sentence.size(); ++i) {
      if (ref.GetParents()[i] == hyp.GetParents()[i])
        ++correct_count;
    }
    return correct_count;
  }

private:
  friend class boost::serialization::access;
  template<class Archive>
  void serialize(Archive & ar, const unsigned int version) {
    ar & options;
    ar & vocab;
    ar & pretrained;
    FinalizeVocab(); // finalize *after* vocab & pretrained to make load work
    ar & model;
  }

  static void OutputConll(const std::vector<unsigned>& sentence,
                          const std::vector<unsigned>& pos,
                          const std::vector<std::string>& sentenceUnkStrings,
                          const std::vector<std::string>& intToWords,
                          const std::vector<std::string>& intToPos,
                          const std::map<std::string, unsigned>& wordsToInt,
                          const ParseTree& tree);
};

} // namespace lstm_parser

#endif // #ifndef LSTM_PARSER_H
