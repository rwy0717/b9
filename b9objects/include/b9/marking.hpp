#if !defined(B9_MARKING_HPP_)
#define B9_MARKING_HPP_

#include <MarkingScheme.hpp>
#include <b9/traverse.hpp>

namespace b9 {

class Marker : public Visitor {
 public:
  Marker(MM_MarkingScheme* omrMarker) : omrMarker_(omrMarker) {}

  virtual void edge(Context& cx, Cell* a, Cell* b) override {
    omrMarker_->markObject(cx.omrGcThread(), b);
  }

  virtual void rootEdge(Context& cx, void* a, Cell* b) override {
    if (b != nullptr)
      omrMarker_->markObject(cx.omrGcThread(), b, true);
  }

  MM_MarkingScheme* omrMarker_;
};

}  // namespace b9

#endif  // B9_MARKING_HPP_
