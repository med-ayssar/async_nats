import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";

export default defineConfig({
  integrations: [
    starlight({
      title: "AsyncNats",
      description: "C++23 NATS client. The library owns main and runs co_main.",
      lastUpdated: false,
      sidebar: [
        { label: "Overview", link: "/" },
        { label: "API", link: "/api/" },
        { label: "Doxygen", link: "/doxygen/" },
        { label: "How it runs", link: "/runtime/" },
        { label: "Core", link: "/core/" },
        { label: "Core implementation", link: "/implementation/" },
        { label: "Client", link: "/client/" },
        { label: "Errors and signals", link: "/errors/" },
        { label: "JetStream", link: "/jetstream/" },
      ],
    }),
  ],
});
