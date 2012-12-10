/**
 * @file ProductRankParam.h
 * @brief the parameters used in product ranking.
 * @author Jun Jiang
 * @date Created 2012-11-30
 */

#ifndef SF1R_PRODUCT_RANK_PARAM_H
#define SF1R_PRODUCT_RANK_PARAM_H

#include <common/inttypes.h>
#include <vector>

namespace sf1r
{

struct ProductRankParam
{
    std::vector<docid_t>& docIds_;

    std::vector<score_t>& topKScores_;

    const std::size_t docNum_;

    ProductRankParam(
        std::vector<docid_t>& docIds,
        std::vector<score_t>& topKScores)
        : docIds_(docIds)
        , topKScores_(topKScores)
        , docNum_(docIds.size())
    {}

    bool isValid() const
    {
        return docNum_ == topKScores_.size();
    }
};

} // namespace sf1r

#endif // SF1R_PRODUCT_RANK_PARAM_H