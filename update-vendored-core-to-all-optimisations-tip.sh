git -C vendor/NeuralAmpModelerCore fetch origin all-optimisations
git -C vendor/NeuralAmpModelerCore log --oneline HEAD..FETCH_HEAD   # what you would move to
git submodule update --remote vendor/NeuralAmpModelerCore           # branch tip into the working tree
git add vendor/NeuralAmpModelerCore
git commit -m "vendor: NAM core to the all-optimisations tip (<sha>)"


