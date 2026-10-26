// nn::ngc::ProfanityFilter, without the system's word lists (system data NgWord2, 0100000000000823, is a part of the
// Switch's firmware, not of a game): it initializes, and finds no profanity in anything - names and messages a game
// checks are accepted as typed, as with the filter turned off.
#include "../hle.h"

// Result ProfanityFilter::Initialize(void *work, size_t size, bool check_desired_language)
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_initialize) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEb", ngc_initialize_nowork) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ngc_finalize) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", ngc_set_mask_mode) { (void)c; }

// Result CheckProfanityWords(u32 *results, u32 patterns, const char16_t **words, size_t count): no pattern matched
static void check_words(Cpu *c) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[1];
    for (uint64_t i = 0; out && i < c->x[4]; i++) out[i] = 0;
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check_words16) { check_words(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", ngc_check_words_u16) { check_words(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check_words8) { check_words(c); }

// Result MaskProfanityWordsInText(int *masked, char16_t *text, u32 patterns): nothing masked
static void mask_text(Cpu *c) {
    int32_t *n = (int32_t *)(uintptr_t)c->x[1];
    if (n) *n = 0;
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", ngc_mask16) { mask_text(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ngc_mask_u16) { mask_text(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ngc_mask8) { mask_text(c); }
