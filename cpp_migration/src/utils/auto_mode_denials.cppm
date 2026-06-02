module;

#include <string>
#include <vector>

export module cc.utils.auto_mode_denials;

export namespace cc::utils::auto_mode_denials {

struct AutoModeDenial {
    std::string tool_name;
    std::string display;
    std::string reason;
    long long timestamp = 0;
};

class AutoModeDenialStore {
public:
    static constexpr std::size_t max_denials = 20;

    void record(const AutoModeDenial& denial, bool transcript_classifier_enabled) {
        if (!transcript_classifier_enabled) return;
        denials_.insert(denials_.begin(), denial);
        if (denials_.size() > max_denials) denials_.resize(max_denials);
    }

    [[nodiscard]] const std::vector<AutoModeDenial>& get() const noexcept {
        return denials_;
    }

    void clear() noexcept {
        denials_.clear();
    }

private:
    std::vector<AutoModeDenial> denials_;
};

} // namespace cc::utils::auto_mode_denials
