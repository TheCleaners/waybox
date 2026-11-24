#include "waybox/xml_strict.hpp"

#include <libxml/parser.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlversion.h>

namespace wb {

namespace {

void take_error(XmlError &out, const xmlError *e) {
	out.message = (e != nullptr && e->message != nullptr) ? e->message : "";
	while (!out.message.empty() &&
			(out.message.back() == '\n' || out.message.back() == ' '))
		out.message.pop_back();
	out.line = e != nullptr ? e->line : 0;
}

}  // namespace

xmlDocPtr read_xml_strict(const char *path, XmlError *error, const char *data,
		int size) {
	xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
	if (ctxt == nullptr)
		return nullptr;
	struct First {
		XmlError err;
		bool set = false;
	} first;
#if LIBXML_VERSION >= 21300
	/* Keep the first error, and keep libxml2 from printing them. */
	xmlCtxtSetErrorHandler(ctxt, [](void *data, const xmlError *e) {
		auto *f = static_cast<First *>(data);
		if (f->set || e == nullptr || e->level < XML_ERR_ERROR)
			return;
		take_error(f->err, e);
		f->set = true;
	}, &first);
#endif
	const int options = XML_PARSE_NONET;
	xmlDocPtr doc = data != nullptr
		? xmlCtxtReadMemory(ctxt, data, size, path, nullptr, options)
		: xmlCtxtReadFile(ctxt, path, nullptr, options);
#if LIBXML_VERSION < 21300
	/* Older libxml2: no per-context handler; take its last error. */
	if (const xmlError *e = xmlCtxtGetLastError(ctxt);
			e != nullptr && e->level >= XML_ERR_ERROR) {
		take_error(first.err, e);
		first.set = true;
	}
#endif
	bool ok = doc != nullptr && ctxt->wellFormed && !first.set &&
			xmlDocGetRootElement(doc) != nullptr;
	xmlFreeParserCtxt(ctxt);
	if (!ok) {
		if (doc != nullptr)
			xmlFreeDoc(doc);
		if (error != nullptr) {
			*error = first.err;
			if (error->message.empty())
				error->message = "not an XML document";
		}
		return nullptr;
	}
	return doc;
}

}  // namespace wb
