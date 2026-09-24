#ifndef LILYGO_UI_CAMERA_LV_SUBJECT_HPP
#define LILYGO_UI_CAMERA_LV_SUBJECT_HPP

#include <lvgl.h>

#include <cstdint>

class IntSubject {
public:
    explicit IntSubject(std::int32_t value, std::int32_t minimum = INT32_MIN, std::int32_t maximum = INT32_MAX)
    {
        lv_subject_init_int(&subject_, value);
        lv_subject_set_min_value_int(&subject_, minimum);
        lv_subject_set_max_value_int(&subject_, maximum);
    }

    ~IntSubject()
    {
        lv_subject_deinit(&subject_);
    }

    IntSubject(const IntSubject &)            = delete;
    IntSubject &operator=(const IntSubject &) = delete;
    IntSubject(IntSubject &&)                 = delete;
    IntSubject &operator=(IntSubject &&)      = delete;

    void set(std::int32_t value)
    {
        lv_subject_set_int(&subject_, value);
    }

    [[nodiscard]] std::int32_t value() const
    {
        return lv_subject_get_int(const_cast<lv_subject_t *>(&subject_));
    }

    [[nodiscard]] lv_subject_t *get() noexcept
    {
        return &subject_;
    }

private:
    lv_subject_t subject_{};
};

#endif
