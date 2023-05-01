# Anitra

Anitra is a lightweight, user-friendly engine skeleton builder designed to bootstrap small game engines, automate the configuration of libraries such as SDL, GLFW, and more, and create a data-oriented structure. Anitra provides real-time DLL reloading functionality, watching for source file changes and recompiling and reloading at runtime. The main purpose of this project is to learn and explore data-oriented design and the C programming language.

## Features

* Engine skeleton builder similar to JavaScript builders
* Automated library configuration for SDL, GLFW, and other game development libraries
* Real-time DLL reloading for immediate feedback during development
* Library watcher and builder for seamless integration and updates
* Customizable settings for different game engine requirements
* Built with data-oriented design principles and C language for optimized performance

## Getting Started

To use Anitra, follow the steps below:

### Prerequisites

Ensure you have the following installed on your system:

* C compiler (e.g., GCC or Clang)
* Build system (e.g., Make or CMake)

### Installation

1. Clone the Anitra repository:

```bash
git clone https://github.com/andresfelipemendez/anitra
```

2. Navigate to the Anitra directory and build with CMake:

```bash
mkdir build
cd build
cmake ..
```

3. Follow the on-screen prompts to configure libraries and settings for your game engine.

## Usage

Once Anitra is installed and configured, you can start using it to bootstrap your game engine and work with the configured libraries.

To enable real-time DLL reloading, ensure your project is set up to generate DLL files for the components you'd like to reload at runtime. Anitra will monitor the specified source files for changes and automatically recompile and reload the associated DLLs.

For more information on customizing Anitra's settings and features, please refer to the [documentation](docs/) (pending).

## Contributing

If you'd like to contribute to Anitra, please follow these steps:

1. Fork the repository.
2. Create a new branch for your feature or bugfix.
3. Make your changes, ensuring you adhere to the project's coding standards.
4. Commit your changes and create a pull request.

For more detailed instructions, please refer to the [contributing guide](CONTRIBUTING.md).

## License

Anitra is released under the [MIT License](LICENSE).

## Contact

For questions or support, please [open an issue](https://github.com/andresfelipemendez/anitra/issues) on GitHub.