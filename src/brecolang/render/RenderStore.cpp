#include "brecolang/render/RenderStore.h"

namespace breco::lang {

RenderStore::RenderStore(std::shared_ptr<const BrecoProgram> program,
                         std::shared_ptr<const DecodedTree> tree,
                         QVector<std::shared_ptr<ByteSource>> inputs,
                         DecodedValueId rootValue)
    : m_program(std::move(program)), m_tree(std::move(tree)),
      m_inputs(std::move(inputs)), m_rootValue(rootValue) {
    if (m_rootValue == kInvalidId && m_tree && !m_tree->nodes.isEmpty()) {
        m_rootValue = m_tree->nodes.first().value;
    }
}

ByteSource* RenderStore::input(InputId id) const {
    return id < static_cast<InputId>(m_inputs.size()) ? m_inputs.at(id).get()
                                                      : nullptr;
}

QString RenderStore::inputRole(InputId id) const {
    if (!m_program || id >= static_cast<InputId>(m_program->inputs.size())) {
        return {};
    }
    return m_program->symbol(m_program->inputs.at(id).name);
}

QString RenderStore::inputPath(InputId id) const {
    ByteSource* source = input(id);
    return source != nullptr ? source->path() : QString();
}

quint64 RenderStore::logicalOffset(const ByteSpanValue& span) const {
    ByteSource* source = input(span.input);
    if (source == nullptr) {
        return span.offset;
    }
    const quint64 base = source->absoluteOffset(0);
    return span.offset >= base ? span.offset - base : 0;
}

}  // namespace breco::lang
