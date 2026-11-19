#pragma once

#include <string_view>

namespace htmlppt {

// A run id names a directory under %TEMP% (htmlppt-<run-id>) and several Win32
// objects (event, pipe, black box), so it must be a single safe path component.
// The supervisor deletes that temp directory with remove_all on every exit
// path, which makes an unchecked id a path-traversal delete.
//
// Allowed: ASCII letters, digits, '.', '_' and '-'. Length is capped at 128 so
// the id also fits the Win32 object-name and path limits. "." and ".." are
// rejected even though their characters are otherwise allowed.
bool IsValidRunId(std::string_view run_id);

} // namespace htmlppt
