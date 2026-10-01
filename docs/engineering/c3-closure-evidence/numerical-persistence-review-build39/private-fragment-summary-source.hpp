namespace qcae::ipc { namespace {
using operations::Value; using operations::canonical_value;
Value validation_summary(const detail::SolverValidationFact& fact, std::string_view identity) {
    std::uint64_t mismatched{};
    for (const auto& component : fact.report.components)
        if (!component.matched)
            ++mismatched;
    return Value(Value::Object{
        {"validation_id", Value(std::string(identity))},
        {"ordinal", Value(std::to_string(fact.ordinal))},
        {"run_id", Value(fact.run_id)},
        {"result_id", Value(fact.result_id)},
        {"task_id", Value(fact.task_id)},
        {"source_kind", Value(fact.test_only ? "test_process" : "external_solver")},
        {"check_kind", Value(fact.test_only ? "synthetic_comparison" : "numerical_validation")},
        {"numerical_stage", Value(detail::solver_validation_numerical_stage(fact))},
        {"comparison", Value(fact.report.matched ? "matched" : "mismatch")},
        {"component_count", Value(std::to_string(fact.report.components.size()))},
        {"mismatched_components", Value(std::to_string(mismatched))},
        {"reference_id", Value(fact.report.reference_id)},
        {"reference_input", Value(fact.report.reference_input)},
        {"original_parsed_sha256", Value(fact.parsed_sha256)},
        {"original_result_manifest_sha256", Value(fact.manifest_sha256)}});
}
struct ValidationHistory {
    std::shared_ptr<const OwnedRowImage> index;
    std::vector<std::pair<detail::SolverValidationFact, std::shared_ptr<const OwnedRowImage>>>
        reports;
    bool overflow{};
};
Value numerical_checks(const ValidationHistory& history, bool current, bool files_verified) {
    Value::Array summaries;
    for (const auto& entry : history.reports)
        summaries.push_back(validation_summary(entry.first, entry.second->key.identity));
    Value latest;
    if (!history.overflow && !history.reports.empty())
        latest = validation_summary(history.reports.back().first,
                                    history.reports.back().second->key.identity);
    constexpr std::size_t summary_byte_limit = 32768;
    const auto encoded = canonical_value(Value(summaries));
    const bool byte_overflow = !encoded.ok() || encoded.value->size() > summary_byte_limit;
    const bool overflow = history.overflow || byte_overflow;
    if (overflow) {
        summaries.clear();
        latest = Value();
    }
    const bool applicable = !overflow && !history.reports.empty();
    return Value(Value::Object{
        {"original_run_validation_stage", Value("not_run")},
        {"original_source_state", Value(current ? "current" : "stale")},
        {"current_files_verified", Value(files_verified)},
        {"latest_applicable_summary", std::move(latest)},
        {"has_applicable_validation", Value(applicable)},
        {"applies_to_current_input", Value(applicable && current)},
        {"immutable_history", Value(std::move(summaries))},
        {"history_limit", Value(std::to_string(detail::solver_validation_history_limit))},
        {"history_summary_byte_limit", Value(std::to_string(summary_byte_limit))},
        {"history_overflow", Value(overflow)},
        {"history_overflow_reason",
         Value(history.overflow ? "row_count"
               : byte_overflow  ? "summary_bytes"
                                : "")}});
}

} }
