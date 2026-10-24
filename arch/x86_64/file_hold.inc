/* N3.6 Gate C: read_at's publication hold, included at statement position after the read's private
 * result and before the final cancellation check. A directive line adds no token, code or line to any
 * other image (production preprocessing and debug lines are unchanged). */
#if defined(VOS_TEST) && defined(VOS_TEST_CASE_storage_stress_c)
    FILE_PUBLICATION_HOLD(r)
#endif
