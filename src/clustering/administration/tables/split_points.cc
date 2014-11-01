#include "clustering/administration/tables/split_points.hpp"

#include "clustering/administration/real_reql_cluster_interface.hpp"
#include "math.hpp"   /* for `clamp()` */
#include "rdb_protocol/real_table.hpp"

/* `interpolate_key()` produces a `store_key_t` that is interpolated between `in1` and
`in2`. For example, if `fraction` is 0.50, the return value will be halfway between `in1`
and `in2`; if it's 0.25, the return value will be closer to `in1`; and so on. This
function is not exact; the only absolute guarantee it provides is that the return value
will lie in the range `in1 <= out <= in2`. */
static store_key_t interpolate_key(store_key_t in1, store_key_t in2, double fraction) {
    rassert(in1 <= in2);
    rassert(fraction >= 0 && fraction <= 1);
    const uint8_t *in1_buf = in1.contents(), *in2_buf = in2.contents();
    uint8_t out_buf[MAX_KEY_SIZE];

    /* Calculate the shared prefix of `in1` and `in2` */
    size_t i = 0;
    while (i < static_cast<size_t>(in1.size()) &&
           i < static_cast<size_t>(in2.size()) &&
           in1_buf[i] == in2_buf[i]) {
        out_buf[i] = in1_buf[i];
        ++i;
    }

    /* Convert the first non-shared parts of `in1` and `in2` into integers so we can do
    arithmetic on them. If `in1` or `in2` terminates early, pad it with zeroes. This
    isn't perfect but the error doesn't matter for our purposes. */
    uint32_t in1_tail = 0, in2_tail = 0;
    static const size_t num_interp = 4;
    rassert(sizeof(in1_tail) >= num_interp);
    for (size_t j = i; j < i + num_interp; ++j) {
        uint8_t c1 = j < static_cast<size_t>(in1.size()) ? in1_buf[j] : 0;
        uint8_t c2 = j < static_cast<size_t>(in2.size()) ? in2_buf[j] : 0;
        in1_tail = (in1_tail << 8) + c1;
        in2_tail = (in2_tail << 8) + c2;
    }
    rassert(in1_tail <= in2_tail);

    /* Compute an integer representation of the interpolated value */
    uint32_t out_tail = in1_tail*(1-fraction) + in2_tail*fraction;

    /* Append the interpolated value onto `out_buf` */
    for (size_t k = 0; k < num_interp; ++k) {
        uint8_t c = (out_tail >> (8 * (num_interp - k))) & 0xFF;
        if (i + k < MAX_KEY_SIZE) {
            out_buf[i + k] = c;
        }
    }

    /* Construct the final result */
    store_key_t out(
        std::min(i + num_interp, static_cast<size_t>(MAX_KEY_SIZE)),
        out_buf);

    /* For various reasons (rounding errors, corner cases involving keys very close
    together, etc.), it's possible that the above procedure will produce an `out` that is
    not between `in1` and `in2`. Rather than trying to interpolate properly in these
    complicated cases, we just clamp the result. */
    out = clamp(out, in1, in2);

    return out;
}

/* `ensure_distinct()` ensures that all of the `store_key_t`s in the given vector are
distinct from eachother. Initially, they should be non-strictly monotonically increasing;
upon return, they will be strictly monotonically increasing. */
static void ensure_distinct(std::vector<store_key_t> *split_points) {
    for (size_t i = 1; i < split_points->size(); ++i) {
        /* Make sure the initial condition is met */
        guarantee(split_points->at(i) >= split_points->at(i-1));
    }
    for (size_t i = 1; i < split_points->size(); ++i) {
        /* Normally, we fix any overlaps by just pushing keys forward. */
        while (split_points->at(i) <= split_points->at(i-1)) {
            bool ok = split_points->at(i).increment();
            if (!ok) {
                /* Oops, we ran into the maximum possible key. This is incredibly
                unlikely in practice, but we handle it anyway. */
                size_t j = i;
                while (j > 1 && split_points->at(j) == split_points->at(j-1)) {
                    bool ok2 = split_points->at(j-1).decrement();
                    guarantee(ok2);
                    --j;
                }
            }
        }
    }
}

bool fetch_distribution(
        const namespace_id_t &table_id,
        real_reql_cluster_interface_t *reql_cluster_interface,
        signal_t *interruptor,
        std::map<store_key_t, int64_t> *counts,
        std::string *error_out) {
    namespace_interface_access_t ns_if_access =
        reql_cluster_interface->get_namespace_repo()->get_namespace_interface(
            table_id, interruptor);
    static const int depth = 2;
    static const int limit = 128;
    distribution_read_t inner_read(depth, limit);
    read_t read(inner_read, profile_bool_t::DONT_PROFILE);
    read_response_t resp;
    try {
        ns_if_access.get()->read_outdated(read, &resp, interruptor);
    } catch (cannot_perform_query_exc_t) {
        *error_out = "Cannot calculate balanced shards because the table isn't "
            "currently available for reading.";
        return false;
    }
    *counts = std::move(
        boost::get<distribution_read_response_t>(resp.response).key_counts);
    return true;
}

bool calculate_split_points_with_distribution(
        const std::map<store_key_t, int64_t> &counts,
        size_t num_shards,
        table_shard_scheme_t *split_points_out,
        std::string *error_out) {
    std::vector<std::pair<int64_t, store_key_t> > pairs;
    int64_t total_count = 0;
    for (auto const &pair : counts) {
        if (pair.second != 0) {
            pairs.push_back(std::make_pair(total_count, pair.first));
        }
        total_count += pair.second;
    }
    if (pairs.size() < static_cast<size_t>(num_shards)) {
        *error_out = strprintf("There isn't enough data in the table to create %zu "
            "balanced shards.", num_shards);
        return false;
    }

    split_points_out->split_points.clear();
    size_t left_pair = 0;
    for (size_t split_index = 1; split_index < num_shards; ++split_index) {
        int64_t split_count = (split_index * total_count) / num_shards;
        rassert(pairs[left_pair].first <= split_count);
        while (left_pair+1 < pairs.size() &&
                pairs[left_pair+1].first <= split_count) {
            ++left_pair;
        }
        std::pair<int64_t, store_key_t> left = pairs[left_pair];
        std::pair<int64_t, store_key_t> right =
            (left_pair == pairs.size() - 1)
                ? std::make_pair(total_count, store_key_t::max())
                : pairs[left_pair+1];
        store_key_t split_key = interpolate_key(left.second, right.second,
            (split_count - left.first) / static_cast<double>(right.first - left.first));
        split_points_out->split_points.push_back(split_key);
    }
    ensure_distinct(&split_points_out->split_points);

    return true;
}

store_key_t key_for_uuid(uint64_t first_8_bytes) {
    uuid_u uuid;
    bzero(uuid.data(), uuid_u::kStaticSize);
    for (size_t i = 0; i < 8; i++) {
        /* Copy one byte at a time to avoid endianness issues */
        uuid.data()[i] = (first_8_bytes >> (8 * (7 - i))) & 0xFF;
    }
    debugf("key_for_uuid %" PRIu64 " -> %s\n", first_8_bytes, uuid_to_str(uuid).c_str());
    return store_key_t(ql::datum_t(datum_string_t(uuid_to_str(uuid))).print_primary());
}

/* `check_distribution_might_be_uuids` checks if the given key counts are consistent with
the hypothesis that the primary keys are all UUIDs. */
bool check_distribution_might_be_uuids(
        const std::map<store_key_t, int64_t> &counts) {
    for (auto it = counts.begin(); it != counts.end();) {
        auto jt = it;
        ++it;
        if (jt->second != 0) {
            if (jt->first > key_for_uuid(std::numeric_limits<uint64_t>::max()) ||
                    (it != counts.end() && it->first < key_for_uuid(0))) {
                return false;
            }
        }
    }
    return true;
}

/* `calculate_split_points_for_uuids` generates a set of split points that will divide
the range of UUIDs evenly. */
void calculate_split_points_for_uuids(
        size_t num_shards,
        table_shard_scheme_t *split_points_out) {
    debugf("calculate_split_points_for_uuids\n");
    split_points_out->split_points.clear();
    for (size_t i = 0; i < num_shards-1; ++i) {
        split_points_out->split_points.push_back(key_for_uuid(
            (std::numeric_limits<uint64_t>::max() / num_shards) * (i+1)));
        guarantee(i == 0 ||
            split_points_out->split_points[i] > split_points_out->split_points[i-1]);
    }
}

/* In practice this will only ever be used to decrease the number of shards, but it still
works correctly whether the number of shards is increased, decreased, or stays the same.
*/
void calculate_split_points_by_interpolation(
        size_t num_shards,
        const table_shard_scheme_t &old_split_points,
        table_shard_scheme_t *split_points_out) {
    /* Short circuit this case because it's both trivial and common */
    if (num_shards == old_split_points.num_shards()) {
        *split_points_out = old_split_points;
        return;
    }
    split_points_out->split_points.clear();
    for (size_t split_index = 1; split_index < num_shards; ++split_index) {
        double split_old_index = split_index *
            (old_split_points.num_shards() / static_cast<double>(num_shards));
        guarantee(split_old_index >= 0);
        guarantee(split_old_index <= old_split_points.num_shards());
        size_t left_old_index = floor(split_old_index);
        guarantee(left_old_index <= old_split_points.num_shards() - 1);
        store_key_t left_key =
            (left_old_index == 0)
                ? store_key_t::min()
                : old_split_points.split_points[left_old_index-1];
        store_key_t right_key =
            (left_old_index == old_split_points.num_shards() - 1)
                ? store_key_t::max()
                : old_split_points.split_points[left_old_index];
        store_key_t split_key = interpolate_key(left_key, right_key,
            split_old_index-left_old_index);
        split_points_out->split_points.push_back(split_key);
    }
    ensure_distinct(&split_points_out->split_points);
}

bool calculate_split_points_intelligently(
        namespace_id_t table_id,
        real_reql_cluster_interface_t *reql_cluster_interface,
        size_t num_shards,
        const table_shard_scheme_t &old_split_points,
        signal_t *interruptor,
        table_shard_scheme_t *split_points_out,
        std::string *error_out) {
    if (num_shards > old_split_points.num_shards()) {
        std::map<store_key_t, int64_t> counts;
        if (!fetch_distribution(table_id, reql_cluster_interface,
                interruptor, &counts, error_out)) {
            return false;
        }
        std::string dummy_error;
        if (!calculate_split_points_with_distribution(
                counts, num_shards, split_points_out, error_out)) {
            /* There aren't enough documents to calculate distribution. We'll have to
            fall back on something else. */
            if (check_distribution_might_be_uuids(counts)) {
                calculate_split_points_for_uuids(num_shards, split_points_out);
            } else {
                calculate_split_points_by_interpolation(
                    num_shards, old_split_points, split_points_out);
            }
        }
    } else if (num_shards == old_split_points.num_shards()) {
        *split_points_out = old_split_points;
    } else {
        calculate_split_points_by_interpolation(
            num_shards, old_split_points, split_points_out);
    }
    return true;
}
