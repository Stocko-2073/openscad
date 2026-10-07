#pragma once

#include <memory>

class AbstractNode;

// Reset to 0 in _prep() and increased for each Node instance in progress_prepare()
extern int progress_report_count;

extern void (*progress_report_f)(const std::shared_ptr<const AbstractNode>&, void *, int);
extern void *progress_report_userdata;

void progress_report_prep(const std::shared_ptr<AbstractNode>& root,
                          void (*f)(const std::shared_ptr<const AbstractNode>& node, void *userdata,
                                    int mark),
                          void *userdata);
void progress_report_fin();
void progress_update(const std::shared_ptr<const AbstractNode>& node, int mark);
// CGALUtils::applyUnion3D may process nodes out of order, so allow for an increment instead of tracking
// exact node
void progress_tick();

// Whether this thread's evaluations leave progress unreported; see ProgressSuppressGuard.
extern thread_local bool progress_report_suppressed;

// Keeps the evaluations on this thread, while it lives, from reporting progress. The report function
// is the render's that set it up (the GUI's F6, reporting to its progress bar and taking its cancel
// button), and Animate's frame workers evaluate on other threads beside it.
class ProgressSuppressGuard
{
public:
  ProgressSuppressGuard() : prev_(progress_report_suppressed) { progress_report_suppressed = true; }
  ~ProgressSuppressGuard() { progress_report_suppressed = prev_; }
  ProgressSuppressGuard(const ProgressSuppressGuard&) = delete;
  ProgressSuppressGuard& operator=(const ProgressSuppressGuard&) = delete;

private:
  bool prev_;
};

class ProgressCancelException
{
};
