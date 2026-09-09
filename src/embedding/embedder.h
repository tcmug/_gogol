#pragma once
#include <string>
#include <vector>

class Embedder {
public:
    explicit Embedder(const std::string &model_path);
    ~Embedder();

    std::vector<float> embed_document(const std::string &text);
    std::vector<float> embed_query(const std::string &text);
    std::vector<std::vector<float>>
    embed_documents_batch(const std::vector<std::string> &texts);
    int dim() const;

private:
    std::vector<float> embed(const std::string &text);
    struct llama_model *model_ = nullptr;
    struct llama_context *ctx_ = nullptr;
    int n_embd_ = 0;
    int n_ctx_ = 0;
};
