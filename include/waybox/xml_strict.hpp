#ifndef WB_XML_STRICT_HPP
#define WB_XML_STRICT_HPP

#include <string>

#include <libxml/tree.h>

namespace wb {

/* Why a document couldn't be read: libxml2's message and its line. */
struct XmlError {
	std::string message;
	int line = 0;
};

/*
 * Parse an XML file (or, with `data`, a buffer) strictly: no error recovery
 * and no network access. libxml2's recover mode "repairs" a broken file by
 * dropping whatever it can't make sense of (one missing quote can take
 * every mouse binding with it), so configuration is read strictly and a
 * broken file is refused with the first error instead. Returns nullptr on
 * any error, filling `error`.
 */
xmlDocPtr read_xml_strict(const char *path, XmlError *error,
		const char *data = nullptr, int size = 0);

}  // namespace wb

#endif /* WB_XML_STRICT_HPP */
