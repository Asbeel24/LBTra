// Renders the editor to a PNG (for reviewing the UI without a DAW).
//   lbtra_snapshot out.png [--more] a.wav b.wav ...
#include "../PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <thread>

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    int rc = 1;
    std::thread t([&] {
        std::unique_ptr<juce::AudioProcessor> base(createPluginFilter());
        auto& p = dynamic_cast<LBTraProcessor&>(*base);
        p.prepareToPlay(48000, 512);
        bool more = false;
        int slot = 0;
        for (int i = 2; i < argc; ++i) {
            juce::String a(argv[i]);
            if (a == "--more") { more = true; continue; }
            juce::String err;
            juce::MessageManager::callAsync([&p, s = slot++, a] { juce::String e; p.loadSlot(s, juce::File(a), e); });
        }
        juce::Thread::sleep(400);
        juce::MessageManager::callAsync([&p] {
            p.params.getParameter("compOn")->setValueNotifyingHost(1.f);
            p.params.getParameter("wideOn")->setValueNotifyingHost(1.f);
            p.rollDice();
        });
        juce::Thread::sleep(500);
        for (int k = 0; k < 200 && (p.isRendering() || !p.current()); ++k) juce::Thread::sleep(50);
        juce::WaitableEvent done;
        juce::MessageManager::callAsync([&] {
            std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
            ed->setSize(760, 460);
            if (more)
                for (auto* c : ed->getChildren())
                    if (c->getWidth() > 300 && !c->isVisible()) c->setVisible(true);
            auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.f);
            juce::File out{juce::String(argv[1])};
            out.deleteFile();
            juce::FileOutputStream os(out);
            juce::PNGImageFormat().writeImageToStream(img, os);
            rc = 0;
            done.signal();
        });
        done.wait(10000);
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    t.join();
    return rc;
}
