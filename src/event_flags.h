#pragma once

#include <cstdint>

namespace sitcom {

class EventFlags {
 public:
  bool Init();
  void Shutdown();
  bool Ready() const { return ready_; }

  bool ReadFlag(std::int32_t flag_id, bool* out_value) const;

 private:
  bool Resolve();
  bool EnsureMan() const;
  int ResolveWorldBlockCategory(std::int32_t flag_id) const;

  mutable bool ready_ = false;
  mutable std::uintptr_t event_flag_man_slot_ = 0;
  mutable std::uintptr_t event_flag_man_ = 0;
  mutable std::uintptr_t field_area_ptr_ = 0;
  mutable bool logged_man_ = false;
};

}  // namespace sitcom
