# 🚀 Media Kit: ExpertOnDevice-AI

This document contains ready-to-use materials to help promote the project on social media, forums, and presentations. Feel free to copy, adapt, and share!

> **Main Hook:** 
> *"Are you tired of paying $20/month per user and having your data train third-party models? ExpertOnDevice-AI brings Sovereignty, Auditing, and Expertise in a $30 ESP32. Not a toy. A Sovereign AI Infrastructure for the Real World."*

---

## 🏛️ The 4 Pillars of Edge AI Manifesto

**ExpertOnDevice-AI** was built on four non-negotiable pillars for modern computing:

1. **Radical Data Sovereignty**: Your data never touches the cloud without your permission. Through a Private Gateway (Ollama/LiteLLM), you can process everything locally, eliminating dependence on public clouds and ensuring total privacy.
2. **Native Auditing**: Absolute transparency. All audio, image, and response data is natively logged to the SD Card. In industrial or healthcare environments, every AI decision is recorded for statutory and institutional compliance.
3. **Total Agnosticism**: No *Vendor Lock-in*. Swap brains (GPT-4o, Llama 3, Groq, Gemini) via Web Portal without recompiling a single line of C code.
4. **Domain Expertise**: Move beyond generic AI. From Agronomist to Engineer, the hardware assumes the desired technical persona through profiles embedded on the SD Card, reducing hallucinations from non-specialized models.

---

## 👔 LinkedIn Post (Corporate and Professional Focus)

**Posting Instructions:** 
We recommend using a demo video of the repository or a clear photo of the device in hand (showcasing its portability).

**Post Text:**

The Hardware that brings AI to the Real (and Private) World 🌐

While the world discusses web chatbots, an open-source framework can transform the ESP32-S3 and the new ESP32-P4 into high-level multimodal (Voice + Vision) assistants.
This project was developed based on 4 critical pillars:

✅ Sovereignty: Run 100% locally or in the cloud. You decide where your data lives.
✅ Auditing: Native logs on the SD Card for compliance and security.
✅ Agnosticism: Change LLMs (GPT, Llama, Groq) via Web Portal without recompiling.
✅ Expertise: Native "Expert" profiles for Industry, Agro, and Health.

A $30 piece of hardware can now be the Engineer, Agronomist, or Tutor that fits in your pocket. Not a toy. A Sovereign AI Infrastructure for the Real World.

👉 Check out the code and architectures on GitHub: https://github.com/marcelinoandrade/assistente-de-IA

#EdgeAI #ESP32 #OpenSource #ArtificialIntelligence #IoT #DigitalSovereignty #Innovation

---

## 💻 Content for Hacker News, Reddit or Technical Forums (Developer Focus)

**Title:** Building a Multimodal AI Assistant with ESP32-P4: Sovereignty and Agnosticism

**Text:**

Hi everyone! I wanted to share the architecture of **ExpertOnDevice-AI**. The challenge was to create something that wasn't just an "API client," but a robust edge AI platform.

Technical Highlights:
- **Media Orchestration**: The ESP32-P4 manages PDM capture, camera ISP, and base64 streaming simultaneously.
- **Memory Management**: Intensive use of PSRAM (up to 32MB) with FreeRTOS to ensure low latency (~5s edge-to-edge).
- **Zero-Touch Config**: Captive portal that saves JSON profiles directly to the SD Card.

The project focuses on **Sovereignty and Auditing**, allowing companies to audit every "thought" of the AI recorded locally, along with **Total Agnosticism**, allowing swapping providers (OpenAI, Groq, Ollama) via a web interface.

Repository link: https://github.com/marcelinoandrade/assistente-de-IA

Feedback on concurrency management on the SPI/SDIO bus is very welcome!
